#ifndef __UI_CORE_H__
#define __UI_CORE_H__

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "lvgl.h"
#include "bt_a2dp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 屏幕尺寸 */
#define TFT_HOR_RES   172   /* 水平分辨率 */
#define TFT_VER_RES   320   /* 垂直分辨率 */

/* 配色 (播放器主界面 / 各 UI 组件共用) */
#define COLOR_BG      lv_color_hex(0x050505)   /* 背景 (近黑) */
#define COLOR_CARD    lv_color_hex(0x111111)   /* 卡片底色 */
#define COLOR_FG      lv_color_hex(0xF0F0F0)   /* 前景/主文字 (近白) */
#define COLOR_MUTED   lv_color_hex(0x888888)   /* 次要文字 (灰) */
#define COLOR_ACCENT  lv_color_hex(0x00D992)   /* 强调色 (绿) */
#define COLOR_BORDER  lv_color_hex(0x2A2A2A)   /* 边框 */
#define COLOR_DIM     lv_color_hex(0x555555)   /* 暗色文字 */

/* UI 共享句柄 (由 ui_core_init 从参数装配) */
extern QueueHandle_t        g_ui_app_cmd_queue;    /* 应用命令队列 (UI 消费) */
extern bt_a2dp_iface_t     *g_ui_bt_iface;         /* 蓝牙接口 */
extern QueueHandle_t        g_ui_audio_cmd_queue;  /* 音频命令队列 */
extern QueueHandle_t        g_ui_audio_rsp_queue;  /* 音频应答队列 */

/* UI 初始化参数 */
typedef struct {
    QueueHandle_t        app_cmd_queue;    /* 应用命令队列 */
    bt_a2dp_iface_t     *bt_iface;         /* 蓝牙接口 */
    QueueHandle_t        audio_cmd_queue;  /* 音频命令队列 */
    QueueHandle_t        audio_rsp_queue;  /* 音频应答队列 */
} ui_params_t;

/* 通用圆形图标按钮 (透明底 + 灰色符号), 供主界面/组件创建控件.
 * parent=父对象, x/y/w/h=位置尺寸, symbol=图标字符. 返回按钮对象. */
lv_obj_t *make_icon_btn(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                        lv_coord_t w, lv_coord_t h, const char *symbol);

/* LVGL 初始化: 装配共享句柄 + 创建主任务 (运行 ui_loop_task).
 * params=UI 参数 */
void ui_core_init(const ui_params_t *params);

/* 显示/输入驱动初始化 (在 ui_loop_task 内调用) */
void ui_core_display_init(void);

/* LVGL 主循环 (ui_core_init 创建的任务主体, 定义于 ui_loop.c) */
void ui_loop_task(void *arg);

/* 触摸开关: 息屏时关闭 (touch_read_cb 短路, 不再读 I2C).
 * enable=true 使能 */
void ui_touch_set_enabled(bool enable);

/* 横屏刷屏的中转缓冲 (2KB, 内部 DMA RAM).
 *
 * 软件旋转时 LVGL 的旋转块可能落在 PSRAM, SPI 驱动就会再要一块同尺寸的 DMA 反射缓冲;
 * WiFi 开着时内部堆早已碎片化, 那块申请必然失败 ("Failed to allocate priv TX buffer").
 * 这块缓冲在 WIFI_EV_READY 申请 (WiFi 已 init 完、列表页还没建, 池子最宽裕),
 * 点「确认」连接前让出去, 流程结束释放.
 * 返回 false 表示申请失败 (横屏刷新可能出错, 但不影响竖屏). */
bool ui_core_rot_stage_acquire(void);
void ui_core_rot_stage_release(void);

#ifdef __cplusplus
}
#endif

#endif
