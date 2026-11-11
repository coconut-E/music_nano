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

static lv_color_t           s_draw_buf1[LVGL_BUF_SIZE];   /* 双缓冲 1 */
static lv_color_t           s_draw_buf2[LVGL_BUF_SIZE];   /* 双缓冲 2 */
static lv_disp_draw_buf_t   s_draw_buf_dsc;   /* LVGL 绘制缓冲描述符 */
static lv_disp_drv_t        s_disp_drv;       /* 显示驱动 */
static lv_indev_drv_t       s_indev_drv;   /* LVGL 8 只存指针不拷贝, 必须常驻 */
static esp_lcd_panel_handle_t s_panel = NULL;   /* LCD 面板句柄 */

/* 触摸开关: 息屏时置 false, touch_read_cb 短路返回 (顺带省 I2C 功耗) */
static volatile bool s_touch_enabled = true;

/* 横屏刷屏用的中转缓冲 (2KB, 内部 DMA RAM).
 *
 * 为什么需要: 软件旋转时 LVGL 的旋转块来自 lv_mem_buf_get() (malloc), 尺寸是
 * LV_DISP_ROT_MAX_BUF (lv_conf.h, 2KB), 可能落在 PSRAM. SPI 的 DMA 读不了 PSRAM,
 * 驱动就会再申请一块同尺寸的 "反射缓冲" (spi_master.c:1214); WiFi 开着时内部堆只剩
 * 几百字节, 那块申请必然失败 → 整页刷不出来 ("Failed to allocate priv TX buffer").
 * 这里自己备一块内部 DMA 缓冲, memcpy 进去再交给 SPI, 就不依赖驱动那块反射缓冲了.
 *
 * 归属: 由设置页的 WiFi 流程管, 不开 WiFi 就完全不占内存.
 *   - 进流程 (WIFI_EV_READY) 申请一次: 此刻 esp_wifi_init 刚结束、列表页还没建,
 *     DMA 池还有 ~12KB, 2KB 随手可得 (不能更早: init 自己要从 DMA 池拿 ~27KB,
 *     0.8.6 用 4KB 静态缓冲抢先占位, 直接把 init 掐死了);
 *   - 点「确认」去连接前让出去: 连接每帧要 ~1.6KB 的 DMA TX 缓冲,
 *     2KB 还占着的话实测只剩 1595B/最大连续块 800B, 第一帧 DISCOVER 都发不出去;
 *   - 流程结束 (ui_wifi_destroy) 释放.
 * 让出去之后不再补: 等列表页建完, 池子只剩 ~900B 连续块, 补的那次必然失败 ——
 * 所以连接失败直接退出整个流程重来 (见 ui_settings_wifi.c 文件头), 而不是回列表.
 * 2KB 够用: LVGL 旋转出的每一块最大就是 LV_DISP_ROT_MAX_BUF.
 * 传输走同步接口, 返回时 DMA 已排空, 所以下一块可以直接覆盖它. */
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
        uint32_t len = w * h * sizeof(lv_color_t);

        if (s_rot_stage && len <= ROT_STAGE_BYTES) {
            memcpy(s_rot_stage, color_p, len);      /* 汇总到内部 DMA RAM */
            lcd_draw_bitmap_sync(area->x1, area->y1, area->x2 + 1, area->y2 + 1, s_rot_stage);
        } else {
            /* 兜底: 超大可走原路 (旋转块正常不会超过 LV_DISP_ROT_MAX_BUF) */
            lcd_draw_bitmap_sync(area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_p);
        }
    } else {
        esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1,   /* 把 area 区域像素送显 */
                                  area->x2 + 1, area->y2 + 1, color_p);
    }
    lv_disp_flush_ready(disp_drv);   /* 通知 LVGL 本次刷新完成 */
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
     * 每块都是一次同步刷屏 —— 见 my_disp_flush 里的说明. */
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
