/* 主菜单页 (APP_MODE_MENU): 三张图标竖排占满整屏 —— 音乐 / 小说 / 设置.
 *
 * 它是"第四种模式", 和音乐/小说平级: 由 app_mode.c 在模式切换的黑场里
 * 调用 ui_menu_build() 建、ui_menu_destroy() 删, 进出都走原有过渡动画.
 * 本文件不碰资源 (音频/封面/蓝牙) 也不碰 NVS —— 那些由 app_mode 的
 * leave/enter 负责. */
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "ui_core.h"
#include "ui_res.h"
#include "app_mode.h"
#include "ui_menu.h"

static const char *TAG = "MENU";

#define ROW_W       TFT_HOR_RES                   /* 172 */
#define ROW_H       104                           /* 104*3 + 4*2 = 320, 正好铺满 */
#define ROW_GAP     4
#define ICON_SIZE   88
#define ICON_X      ((ROW_W - ICON_SIZE) / 2)     /* 42 */
#define ICON_Y      ((ROW_H - ICON_SIZE) / 2)     /* 8 */

/* 三行图标数据, 顺序与菜单从上到下一致: 音乐 / 小说 / 设置 */
static const uint16_t *const k_icon_px[] = {
    icon_menu_music,
    icon_menu_novel,
    icon_menu_settings,
};
#define ROW_COUNT ((int)(sizeof(k_icon_px) / sizeof(k_icon_px[0])))

static lv_obj_t    *s_root;                    /* 整页容器 (NULL=未构建) */
static lv_img_dsc_t s_dsc[ROW_COUNT];          /* 图标描述符 (数据指向 flash 常量数组) */

/* 按下: 行底色亮起 (设置行也有同样的反馈) */
static void row_pressed_cb(lv_event_t *e)
{
    lv_obj_set_style_bg_opa(lv_event_get_current_target(e), LV_OPA_COVER, 0);
}

/* 松手/取消: 底色复原 */
static void row_released_cb(lv_event_t *e)
{
    lv_obj_set_style_bg_opa(lv_event_get_current_target(e), LV_OPA_TRANSP, 0);
}

/* 真正的切换延后到下一轮 lv_timer_handler 执行:
 * 切换会删掉本页 (发生在 mode_anim 的定时器回调里), 不在 LVGL 派发事件的
 * 过程中动对象更稳妥, 也和长按电源键那条路径的执行上下文一致. */
static void menu_switch_async(void *arg)
{
    app_mode_request((app_mode_t)(lv_uintptr_t)arg);
}

static void row_click_cb(lv_event_t *e)
{
    app_mode_t target = (app_mode_t)(lv_uintptr_t)lv_event_get_user_data(e);
    lv_async_call(menu_switch_async, (void *)(lv_uintptr_t)target);
}

/* 建一行: 透明底, 按下变 COLOR_CARD, 图标居中 */
static lv_obj_t *make_row(int idx)
{
    lv_img_dsc_t *d = &s_dsc[idx];
    d->header.cf = LV_IMG_CF_TRUE_COLOR;
    d->header.w  = ICON_SIZE;
    d->header.h  = ICON_SIZE;
    d->data_size = ICON_SIZE * ICON_SIZE * 2;
    d->data      = (const uint8_t *)k_icon_px[idx];

    lv_obj_t *row = lv_obj_create(s_root);
    lv_obj_remove_style_all(row);              /* 从零开始: 去掉主题给的边距/边框/阴影 */
    lv_obj_set_size(row, ROW_W, ROW_H);
    lv_obj_set_pos(row, 0, idx * (ROW_H + ROW_GAP));
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_bg_color(row, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, row_pressed_cb,  LV_EVENT_PRESSED,    NULL);
    lv_obj_add_event_cb(row, row_released_cb, LV_EVENT_RELEASED,   NULL);
    lv_obj_add_event_cb(row, row_released_cb, LV_EVENT_PRESS_LOST, NULL);

    lv_obj_t *img = lv_img_create(row);
    lv_img_set_src(img, d);
    lv_obj_set_pos(img, ICON_X, ICON_Y);

    return row;
}

void ui_menu_build(void)
{
    if (s_root) return;                        /* 幂等: 已建好就不重建 */

    /* 进菜单是这台机器最干净的时刻: app_mode 的 mode_music_leave 已经停播、挂起音频
     * 任务、挂起蓝牙, 既没有 PCM 生产者, 也没有协议栈事件在跑 —— 所以这一行数字最适合
     * 当"内部 RAM 基线"看 (后来 WiFi 流程里那个见底的数字跟它比). */
    ESP_LOGI(TAG, "进菜单: 内部空闲 %u B (最大块 %u), DMA 空闲 %u B (最大块 %u)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));

    s_root = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, TFT_HOR_RES, TFT_VER_RES);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_style_bg_color(s_root, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *row_music = make_row(0);
    lv_obj_t *row_novel = make_row(1);
    lv_obj_t *row_set   = make_row(2);

    lv_obj_add_event_cb(row_music, row_click_cb, LV_EVENT_CLICKED,
                        (void *)(lv_uintptr_t)APP_MODE_MUSIC);
    lv_obj_add_event_cb(row_novel, row_click_cb, LV_EVENT_CLICKED,
                        (void *)(lv_uintptr_t)APP_MODE_NOVEL);
    lv_obj_add_event_cb(row_set,   row_click_cb, LV_EVENT_CLICKED,
                        (void *)(lv_uintptr_t)APP_MODE_SETTINGS);

    lv_obj_move_foreground(s_root);            /* 压住常驻的音量弹窗 */
    ESP_LOGI(TAG, "菜单页已构建");
}

void ui_menu_destroy(void)
{
    if (!s_root) return;
    lv_obj_del(s_root);
    s_root = NULL;
    ESP_LOGI(TAG, "菜单页已销毁");
}
