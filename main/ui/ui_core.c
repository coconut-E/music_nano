#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "drv_display.h"
#include "touch.h"
#include "ui_core.h"
#include "settings.h"
#include "power_mgr.h"

#define TAG "ui_core"

/* 绘制缓冲总大小: 屏的 60% (两块各 30%), 单块 172x96, 整屏仍是 4 条带 (96/96/96/32).
 *
 * 65% → 60% 是为了再给设置页的 esp_wifi_init 让出 5,504 B 内部 DMA 堆 (WiFi 要从
 * DMA 池拿 ~27KB, 而横屏页还有旋转临时块 + SPI 反射块要抢). 60% 和 65% 一样是
 * 4 条带, 只是每条窄一点; 40% 会变 5 条带, 分带接缝明显变多, 那才是撕裂变差的原因.
 * 注意: 绘制缓冲必须留在内部 RAM —— ESP32 的 SPI DMA 读不了 PSRAM. */
#define DRAW_BUF_SIZE  (TFT_HOR_RES * TFT_VER_RES / 20 * 12)  /* 绘制缓冲总大小 (屏的 60%) */
#define LVGL_BUF_SIZE  (DRAW_BUF_SIZE / 2)                    /* 单帧缓冲 = 一半 */

/* 触摸原始坐标的可用范围 (用于线性映射到屏幕坐标) */
#define TOUCH_Y_MIN  5
#define TOUCH_Y_MAX  310

/* UI 共享句柄 */
QueueHandle_t        g_ui_app_cmd_queue  = NULL;   /* 应用命令队列 */
bt_a2dp_iface_t     *g_ui_bt_iface       = NULL;   /* 蓝牙接口 */
QueueHandle_t        g_ui_audio_cmd_queue = NULL;  /* 音频命令队列 */
QueueHandle_t        g_ui_audio_rsp_queue = NULL;  /* 音频应答队列 */

/* aligned(4): 16 位色深下 lv_color_t 是 2 字节, 数组默认对齐可能只有 2.
 * 第二块缓冲要借给 WiFi 当私有堆 (见下面的 ui_core_draw_buf_borrow),
 * multi_heap 的块头按 size_t 读写, 2 字节对齐会把相邻内存踩坏. */
static lv_color_t           s_draw_buf1[LVGL_BUF_SIZE] __attribute__((aligned(4)));   /* 双缓冲 1 */
static lv_color_t           s_draw_buf2[LVGL_BUF_SIZE] __attribute__((aligned(4)));   /* 双缓冲 2 */
static lv_disp_draw_buf_t   s_draw_buf_dsc;   /* LVGL 绘制缓冲描述符 */
static lv_disp_drv_t        s_disp_drv;       /* 显示驱动 */
static lv_indev_drv_t       s_indev_drv;   /* LVGL 8 只存指针不拷贝, 必须常驻 */
static esp_lcd_panel_handle_t s_panel = NULL;   /* LCD 面板句柄 */

/* 触摸开关: 息屏时置 false, touch_read_cb 短路返回 (顺带省 I2C 功耗) */
static volatile bool s_touch_enabled = true;

/* WiFi 会话期间的单缓冲标记 (见 ui_core_draw_buf_borrow / _return).
 *
 * 为什么单缓冲必须配同步刷新: 竖屏那条分支是"挂进 DMA 队列就 flush_ready",
 * 双缓冲时下一段渲染落到另一块, 没事; 只剩一块时 LVGL 会往 DMA 正在读的那块里
 * 画, 花屏. WiFi 页全是静态的, 丢掉渲染/搬运的重叠看不出来. */
static volatile bool s_flush_sync   = false;
static bool          s_buf_borrowed = false;

/* 横屏刷屏用的中转缓冲 (2KB, 内部 DMA RAM).
 *
 * 为什么需要: 软件旋转时 LVGL 的旋转块来自 lv_mem_buf_get() (malloc), 尺寸是
 * LV_DISP_ROT_MAX_BUF (lv_conf.h, 2KB), 池子紧的时候会落到 IRAM/PSRAM —— 那里的
 * 内存不满足 SPI DMA, 驱动就会再申请一块同尺寸的 "反射缓冲" (spi_master.c:1214);
 * WiFi 开着时内部堆只剩几百字节, 那块申请必然失败 → 整页刷不出来
 * ("Failed to allocate priv TX buffer"). 这里自己备一块内部 DMA 缓冲, memcpy 进去
 * 再交给 SPI, 就不依赖驱动那块反射缓冲了.
 *
 * 为什么坚持 2KB (试过 1KB, 能用但会把 WiFi 饿死):
 *   这块缓冲在连接前会被释放, 释放出来的那一块**正好是 WiFi 要的那块连续空间** ——
 *   WiFi 每发/收一帧要 ~1.6KB 连续的 DMA 缓冲. 用 2KB 时释放后留的是一个 2048 的洞,
 *   连接能起来; 用 1KB 时洞里只有 1024, 实测 "最大连续块 1024" -> 关联成功、DHCP
 *   DISCOVER 发不出去、20 秒拿不到 IP、最后 reason=8 退出.
 *   所以这里的尺寸不只是"够刷屏就行", 还负责给 WiFi 留一个够大的洞.
 *
 * 能不能拿到 2KB: 实测 WIFI_EV_READY 时 DMA 池 free 2755B / 最大连续块 1792B,
 *   差 256B 拿不到 —— 那是 .bss 里 OTA 的大字符串(约 1.5KB)直接从 DRAM 扣掉的缘故.
 *   把那些字符串挪到 PSRAM 之后, 最大连续块回到 3KB 以上, 2048 就能拿到.
 *
 * 归属: 由设置页的 WiFi 流程管, 不开 WiFi 就完全不占内存.
 *   - 进流程 (WIFI_EV_READY) 申请一次 (不能更早: init 自己要从 DMA 池拿 ~27KB,
 *     0.8.6 用 4KB 静态缓冲抢先占位, 直接把 init 掐死了);
 *   - 点「确认」去连接前让出去 (start_connect), 顺便把列表页也删掉, 见那里的说明;
 *   - 流程结束 (ui_wifi_destroy) 释放.
 * 传输走同步接口, 返回时 DMA 已排空, 所以下一段可以直接覆盖它. */
#define ROT_STAGE_BYTES 2048
static uint8_t *s_rot_stage = NULL;

/* 申请横屏中转缓冲: 幂等, 已经有就直接返回 true.
 *
 * 唯一可靠的时机是 WIFI_EV_READY (见上). 密码页可以反复进入
 * (电源键返回 -> 再进), 所以申请放在流程入口, 不是密码页的构建函数里. */
bool ui_core_rot_stage_acquire(void)
{
    if (s_rot_stage) return true;
    s_rot_stage = (uint8_t *)heap_caps_malloc(ROT_STAGE_BYTES,
                                              MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!s_rot_stage) {
        ESP_LOGW(TAG, "横屏中转缓冲申请失败 (DMA 可用 %u B), 横屏刷新会出错",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
        return false;
    }
    ESP_LOGI(TAG, "横屏中转缓冲已申请 %u B (DMA 可用 %u B)",
             (unsigned)ROT_STAGE_BYTES,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    return true;
}

void ui_core_rot_stage_release(void)
{
    if (!s_rot_stage) return;
    heap_caps_free(s_rot_stage);
    s_rot_stage = NULL;
    ESP_LOGI(TAG, "横屏中转缓冲已释放 (DMA 可用 %u B)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
}

/* LVGL 刷新回调: 把脏区像素写进 LCD 面板.
 *
 * 竖屏走异步 (沿用原来的做法). 横屏 (软件旋转 90°) 必须同步:
 * lv_refr.c 的 draw_buf_rotate() 会把同一块缓冲原地旋转 / 反复复用一个 rot_buf,
 * 每块之间只等一个 draw_buf->flushing 标志 —— 而我们这里一旦立即 flush_ready,
 * 它就会接着改写还在被 DMA 读的缓冲, 横屏页会整块串行错位.
 * lcd_draw_bitmap_sync 末尾用一次无命令 tx_param 把在途 color 事务排空, 正是 DMA 屏障. */
static void my_disp_flush(lv_disp_drv_t *disp_drv, const lv_area_t *area,
                          lv_color_t *color_p)
{
    if (lv_disp_get_rotation(NULL) != LV_DISP_ROT_NONE) {
        uint32_t w   = (uint32_t)(area->x2 - area->x1 + 1);
        uint32_t h   = (uint32_t)(area->y2 - area->y1 + 1);
        uint32_t row = w * sizeof(lv_color_t);      /* 面板一行字节数, 最大 2*320 */

        /* 旋转块统一过一遍内部 DMA 缓冲. LVGL 那块 2KB 可能装不下一整段,
         * 所以按"面板行"切: 任意连续几行都是合法矩形, 可以分多次发.
         * 中转缓冲只要装得下一行就行, 与 LV_DISP_ROT_MAX_BUF 解耦. */
        if (s_rot_stage && row && row <= ROT_STAGE_BYTES) {
            uint32_t rows = ROT_STAGE_BYTES / row;      /* 一次能搬几行 (>=1) */
            if (rows > h) rows = h;
            for (uint32_t y = 0; y < h; y += rows) {
                uint32_t n = (y + rows <= h) ? rows : (h - y);
                memcpy(s_rot_stage, color_p + (size_t)y * w, (size_t)n * row);
                lcd_draw_bitmap_sync(area->x1, area->y1 + y,
                                     area->x2 + 1, area->y1 + y + n, s_rot_stage);
            }
        } else {
            /* 单行都放不下, 或缓冲没拿到: 只能原路发, 驱动会自己去要反射缓冲,
             * 要不到就是 RAMWR failed. 正常到不了这里 (申请失败时就不进横屏页). */
            lcd_draw_bitmap_sync(area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_p);
        }
    } else if (s_flush_sync) {
        /* 只有一块绘制缓冲时必须等 DMA 排空才能让 LVGL 继续写这块.
         * lcd_draw_bitmap_sync 末尾用一次无命令 tx_param 排空在途事务, 就是 DMA 屏障
         * (drv_display.c:168). WiFi 会话期间才走到这里, 见 ui_core_draw_buf_borrow. */
        lcd_draw_bitmap_sync(area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_p);
    } else {
        esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1,   /* 把 area 区域像素送显 */
                                  area->x2 + 1, area->y2 + 1, color_p);
    }
    lv_disp_flush_ready(disp_drv);   /* 通知 LVGL 本次刷新完成 */
}

/* ── 绘制缓冲交接: WiFi 会话期间把第二块让出去 ──
 *
 * 是"所有权转移", 不是释放: 两块缓冲始终在 .bss 里, 只是这段时间 LVGL 只认第一块,
 * 第二块由 app/wifi_arena.c 建成私有堆给 WiFi 的分配用. 结束时把描述符改回来即可 ——
 * 一次结构体写入 (lv_hal_disp.c:150-158 只写 buf1/buf2/size), 不需要任何分配,
 * 所以"一定拿得回来", 也不依赖公共堆当时还剩多少连续空间.
 *
 * 只能在 LVGL 任务里调用 (所有 lv_* 都在那儿跑). 调用点是设置页的 WiFi 入口
 * (借用) 和 WIFI_EV_RELEASED 处理 (归还). */

void ui_core_draw_buf_borrow(void **mem, size_t *len)
{
    if (s_buf_borrowed) {                    /* 幂等: 重复进流程不会重复切换 */
        if (mem) *mem = s_draw_buf2;
        if (len) *len = sizeof(s_draw_buf2);
        return;
    }

    /* 先把刷新切到同步, 再排空在途 DMA, 最后才换描述符.
     * 顺序很重要: 竖屏刷屏是异步入队的, 此刻可能刚有一块 buf2 还在被 DMA 读,
     * 直接把它交给 WiFi 就是"一边 DMA 读、一边堆头往里写". */
    s_flush_sync = true;
    lcd_wait_idle();                         /* drv_display.c: 无命令 tx_param 屏障 */
    lv_refr_now(NULL);                       /* 把还挂着的脏区用同步路径刷完 */

    lv_disp_draw_buf_init(&s_draw_buf_dsc, s_draw_buf1, NULL, LVGL_BUF_SIZE);
    s_buf_borrowed = true;
    lv_obj_invalidate(lv_scr_act());         /* 下一帧整屏重画, 不受旧内容影响 */

    if (mem) *mem = s_draw_buf2;
    if (len) *len = sizeof(s_draw_buf2);
    ESP_LOGI(TAG, "绘制缓冲改单缓冲, 交出 %u B 给私有堆", (unsigned)sizeof(s_draw_buf2));
}

void ui_core_draw_buf_return(void)
{
    if (!s_buf_borrowed) return;

    lv_refr_now(NULL);                       /* 此刻还是同步路径, 刷完就是干净的 */
    lcd_wait_idle();

    lv_disp_draw_buf_init(&s_draw_buf_dsc, s_draw_buf1, s_draw_buf2, LVGL_BUF_SIZE);
    s_flush_sync   = false;
    s_buf_borrowed = false;
    lv_obj_invalidate(lv_scr_act());         /* 双缓冲回来了, 整屏重画一次 */

    ESP_LOGI(TAG, "绘制缓冲恢复双缓冲 (%u B/块)", (unsigned)sizeof(s_draw_buf1));
}

void ui_touch_set_enabled(bool enable)
{
    s_touch_enabled = enable;
}

/* LVGL 触摸读取回调 (50Hz): 有中断/按下时才读 I2C, 映射到屏幕坐标 */
static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    if (!s_touch_enabled) {   /* 息屏: 清状态并强制抬起 */
        touch_reset();
        data->state = LV_INDEV_STATE_REL;
        return;
    }

    int32_t tx, ty;
    if (touch_poll(&tx, &ty)) {
        /* 任意触点都算一次操作 (含空容器/无效区), 重置无操作熄屏计时 */
        power_mgr_notify_activity();
        /* 原始 Y 映射到屏幕坐标 */
        ty = (int32_t)(((int)ty - TOUCH_Y_MIN) * (TFT_VER_RES - 1)
                       / (TOUCH_Y_MAX - TOUCH_Y_MIN));
        /* 屏幕安装方向: 水平/垂直都翻转 */
        tx = TFT_HOR_RES - 1 - tx;
        ty = TFT_VER_RES - 1 - ty;
        if (tx >= TFT_HOR_RES) tx = TFT_HOR_RES - 1;   /* 钳制 */
        if (ty >= TFT_VER_RES) ty = TFT_VER_RES - 1;
        data->point.x = (lv_coord_t)tx;
        data->point.y = (lv_coord_t)ty;
        data->state = LV_INDEV_STATE_PR;   /* 按下 */
    } else {
        data->state = LV_INDEV_STATE_REL;   /* 抬起 */
    }
}

/* 创建通用圆形图标按钮: parent=父对象, x/y/w/h=几何, symbol=图标字符 */
lv_obj_t *make_icon_btn(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                        lv_coord_t w, lv_coord_t h,
                        const char *symbol)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);   /* 圆形 */
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(btn, 0, 0);                  /* 透明底 */
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);

    lv_obj_t *icon = lv_label_create(btn);   /* 图标字符 */
    lv_label_set_text(icon, symbol);
    lv_obj_set_style_text_color(icon, COLOR_MUTED, 0);
    lv_obj_center(icon);                    /* 居中 */

    return btn;
}

/* 显示/输入驱动初始化: 注册 LVGL 显示与触摸驱动 */
void ui_core_display_init(void)
{
    lv_init();   /* LVGL 库初始化 */

    s_panel = lcd_get_panel();
    touch_init();   /* 触摸: I2C 总线 + GPIO INT 中断 */

    /* 双缓冲显示驱动 */
    lv_disp_draw_buf_init(&s_draw_buf_dsc, s_draw_buf1, s_draw_buf2, LVGL_BUF_SIZE);
    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res  = TFT_HOR_RES;
    s_disp_drv.ver_res  = TFT_VER_RES;
    s_disp_drv.flush_cb = my_disp_flush;
    s_disp_drv.draw_buf = &s_draw_buf_dsc;
    /* 允许运行期软件旋转 (设置页的 WiFi 密码键盘是横屏): 不打开这个开关,
     * lv_disp_set_rotation() 只会记下角度, 不会真的把像素转过来.
     * 旋转时 LVGL 用 LV_DISP_ROT_MAX_BUF (lv_conf.h, 2KB) 分块转,
     * 每块再由 my_disp_flush 过一遍 1KB 中转缓冲、按面板行分几次同步刷屏 ——
     * 见 my_disp_flush 里的说明. */
    s_disp_drv.sw_rotate = 1;
    lv_disp_drv_register(&s_disp_drv);

    /* 屏底色钉成近黑.
     * LVGL 的默认浅色主题会把 scr 的底色设成白色 (LV_THEME_DEFAULT_DARK 0), 各页面
     * 自己盖一层全屏不透明容器时看不出来; 但切模式的瞬间"旧页已删、新页还没建",
     * 只要那一帧被刷出去, 露出的就是整屏白 —— 设置页切到菜单时那一闪白就是这么来的.
     * 这里把 scr 的底色改成 COLOR_BG, 任何没被盖住的区域都只是黑的. */
    lv_obj_set_style_bg_color(lv_scr_act(), COLOR_BG, 0);
    lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, 0);

    /* 触摸输入驱动 */
    lv_indev_drv_init(&s_indev_drv);
    s_indev_drv.type    = LV_INDEV_TYPE_POINTER;
    s_indev_drv.read_cb = touch_read_cb;
    lv_indev_drv_register(&s_indev_drv);
}

/* UI 初始化: 装配共享句柄, 读设置, 创建 LVGL 主任务 */
void ui_core_init(const ui_params_t *params)
{
    g_ui_app_cmd_queue  = params->app_cmd_queue;
    g_ui_bt_iface       = params->bt_iface;
    g_ui_audio_cmd_queue = params->audio_cmd_queue;
    g_ui_audio_rsp_queue = params->audio_rsp_queue;

    volume_load_from_nvs();       /* 从 NVS 恢复音量 */
    brightness_load_from_nvs();   /* 从 NVS 恢复亮度 */

    xTaskCreatePinnedToCore(ui_loop_task, "lvgl", 6144, NULL, 1, NULL, 1);   /* LVGL 主任务 (core 1) */
}
