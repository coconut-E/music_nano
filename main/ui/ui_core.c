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

#define TAG "ui_core"

#define DRAW_BUF_SIZE  (TFT_HOR_RES * TFT_VER_RES / 10 * 7)   /* 绘制缓冲总大小 (屏的 70%) */
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

/* LVGL 刷新回调: 把脏区像素写进 LCD 面板 */
static void my_disp_flush(lv_disp_drv_t *disp_drv, const lv_area_t *area,
                          lv_color_t *color_p)
{
    esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1,   /* 把 area 区域像素送显 */
                              area->x2 + 1, area->y2 + 1, color_p);
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
    lv_obj_set_style_bg_opa(btn, 8, 0);                  /* 微透明底 */
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
    lv_disp_drv_register(&s_disp_drv);

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

    xTaskCreatePinnedToCore(ui_loop_task, "lvgl", 8192, NULL, 1, NULL, 1);   /* LVGL 主任务 (core 1) */
}
