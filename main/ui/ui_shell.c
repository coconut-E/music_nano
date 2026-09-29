#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "settings.h"
#include "sys_monitor.h"
#include "atomic_utils.h"
#include "ui_core.h"
#include "ui_player.h"
#include "ui_novel.h"
#include "ui_shell.h"
#include "app_mode.h"
#include "menu.h"
#include "audio_task.h"
#include "played_bits.h"
#include "power_mgr.h"
#include "drv_display.h"

extern const lv_font_t lv_font_montserrat_14;

/* ── 按键引脚 ── */
#define PIN_VOL_UP     36
#define PIN_VOL_DOWN   38
#define PIN_PWR_KEY    37                /* 息屏/唤醒按键 (GPIO37, 外部10k下拉) */
#define VOLUME_STEP    4                 /* 音量单步步进 */
#define BRIGHTNESS_STEP 8                /* 小说模式: 亮度单步步进 */
#define VOL_KEY_POLL_MS 10               /* 轮询周期 10ms */
#define VOL_LONGPRESS_MS 600             /* 长按判定: 超过此值进入重复模式 */
#define VOL_REPEAT_MS  150               /* 长按重复步进间隔 */

/* ── 音量/亮度弹窗: (140,45) 30x110, 变化时滑入显示, 2秒无变化滑出隐藏 ── */
#define VOL_POP_X       140
#define VOL_POP_Y       45
#define VOL_POP_W       30
#define VOL_POP_H       110
#define VOL_POP_BAR_H   90
#define VOL_POP_HIDE_MS 2000
#define VOL_POP_ANIM_IN_MS   150
#define VOL_POP_ANIM_OUT_MS  100

/* 弹窗控件 */
static lv_obj_t *s_vol_cont = NULL;   /* 弹窗容器 */
static lv_obj_t *s_vol_bar  = NULL;   /* 进度条 */
static lv_obj_t *s_vol_val  = NULL;   /* 数值标签 */
static int32_t   s_media_last = -1;   /* 上次显示的值 (音量或亮度) */
static int       s_vol_idle = 0;      /* 无变化计时 (ms) */
typedef enum {
    VOL_STATE_HIDDEN,
    VOL_STATE_SHOWING,
    VOL_STATE_HIDING,
} vol_state_t;
static vol_state_t s_vol_state = VOL_STATE_HIDDEN;

/* ── 弹窗动画/显示 ── */
static void vol_popup_create(void)
{
    s_vol_cont = lv_obj_create(lv_scr_act());
    lv_obj_set_pos(s_vol_cont, TFT_HOR_RES, VOL_POP_Y);  /* 初始在屏外, 由动画滑入 */
    lv_obj_set_size(s_vol_cont, VOL_POP_W, VOL_POP_H);
    lv_obj_set_style_bg_color(s_vol_cont, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_vol_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_vol_cont, 6, 0);
    lv_obj_set_style_border_width(s_vol_cont, 0, 0);
    lv_obj_set_style_shadow_width(s_vol_cont, 0, 0);
    lv_obj_set_style_pad_all(s_vol_cont, 0, 0);
    lv_obj_clear_flag(s_vol_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_vol_cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_opa(s_vol_cont, LV_OPA_TRANSP, LV_PART_SCROLLBAR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_vol_cont, LV_OPA_TRANSP, LV_PART_SCROLLBAR | LV_STATE_SCROLLED);

    s_vol_bar = lv_bar_create(s_vol_cont);
    lv_obj_set_pos(s_vol_bar, (VOL_POP_W - 22) / 2, 4);
    lv_obj_set_size(s_vol_bar, 22, VOL_POP_BAR_H);
    lv_obj_set_style_bg_color(s_vol_bar, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_style_bg_opa(s_vol_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_vol_bar, 3, 0);
    lv_obj_set_style_radius(s_vol_bar, 3, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_vol_bar, COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_vol_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_bar_set_range(s_vol_bar, 0, VOLUME_MAX);
    lv_bar_set_value(s_vol_bar, volume_get(), LV_ANIM_OFF);

    s_vol_val = lv_label_create(s_vol_cont);
    lv_obj_set_pos(s_vol_val, 0, 4 + VOL_POP_BAR_H + 2);
    lv_obj_set_size(s_vol_val, VOL_POP_W, 18);
    lv_obj_set_style_text_align(s_vol_val, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_vol_val, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_vol_val, lv_color_black(), 0);
    lv_label_set_text_fmt(s_vol_val, "%" PRId32, volume_get());

    lv_obj_add_flag(s_vol_cont, LV_OBJ_FLAG_HIDDEN);
    s_media_last = volume_get();
}

static void vol_popup_set_x(void *obj, int32_t x)
{
    lv_obj_set_x((lv_obj_t *)obj, (lv_coord_t)x);
}

/* 按当前模式同步弹窗量程/值/文本 (音乐=音量, 小说=亮度), 并记录基线值.
 * 模式切换后必须调用, 避免弹出时仍显示另一模式的残留值/量程. */
static void vol_popup_sync(void)
{
    if (!s_vol_bar || !s_vol_val) return;
    if (app_mode_is_novel()) {
        lv_bar_set_range(s_vol_bar, BRIGHTNESS_MIN, BRIGHTNESS_MAX);
        lv_bar_set_value(s_vol_bar, brightness_get(), LV_ANIM_OFF);
        lv_label_set_text_fmt(s_vol_val, "%" PRId32, (int32_t)brightness_get());
        s_media_last = brightness_get();
    } else {
        lv_bar_set_range(s_vol_bar, VOLUME_MIN, VOLUME_MAX);
        lv_bar_set_value(s_vol_bar, volume_get(), LV_ANIM_OFF);
        lv_label_set_text_fmt(s_vol_val, "%" PRId32, volume_get());
        s_media_last = volume_get();
    }
}

/* 从屏外滑入, overshoot 过冲后回落目标位 */
static void vol_popup_slide_in(void)
{
    vol_popup_sync();   /* 弹出前先恢复当前模式的值 */
    lv_anim_del(s_vol_cont, NULL);
    lv_obj_move_foreground(s_vol_cont);
    lv_obj_clear_flag(s_vol_cont, LV_OBJ_FLAG_HIDDEN);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_vol_cont);
    lv_anim_set_exec_cb(&a, vol_popup_set_x);
    lv_anim_set_values(&a, TFT_HOR_RES, VOL_POP_X);
    lv_anim_set_time(&a, VOL_POP_ANIM_IN_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
    lv_anim_start(&a);
    s_vol_state = VOL_STATE_SHOWING;
}

static void vol_popup_slide_out_end(lv_anim_t *a)
{
    lv_obj_add_flag(s_vol_cont, LV_OBJ_FLAG_HIDDEN);
    s_vol_state = VOL_STATE_HIDDEN;
}

/* 匀速滑出屏外 */
static void vol_popup_slide_out(void)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_vol_cont);
    lv_anim_set_exec_cb(&a, vol_popup_set_x);
    lv_anim_set_values(&a, lv_obj_get_x(s_vol_cont), TFT_HOR_RES);
    lv_anim_set_time(&a, VOL_POP_ANIM_OUT_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_set_ready_cb(&a, vol_popup_slide_out_end);
    lv_anim_start(&a);
    s_vol_state = VOL_STATE_HIDING;
}

/* 弹窗监视 (5Hz): 音乐=音量, 小说=亮度; 变化则滑入并写入 NVS, 2秒无变化滑出 */
static void media_monitor_cb(lv_timer_t *timer)
{
    (void)timer;
    int32_t v;
    int32_t vmin, vmax;

    if (app_mode_is_novel()) {
        v    = brightness_get();
        vmin = BRIGHTNESS_MIN;
        vmax = BRIGHTNESS_MAX;
    } else {
        v    = volume_get();
        vmin = VOLUME_MIN;
        vmax = VOLUME_MAX;
    }

    if (v != s_media_last) {
        s_media_last = v;
        s_vol_idle = 0;
        lv_bar_set_range(s_vol_bar, vmin, vmax);
        lv_bar_set_value(s_vol_bar, v, LV_ANIM_OFF);
        lv_label_set_text_fmt(s_vol_val, "%" PRId32, v);
        if (s_vol_state == VOL_STATE_HIDDEN) {
            vol_popup_slide_in();
        } else if (s_vol_state == VOL_STATE_HIDING) {
            vol_popup_slide_in();   /* 滑出中被调回, 重新滑入 */
        }
        /* 仅在值变化时落盘, 避免每 200ms 擦写 NVS */
        if (app_mode_is_novel()) {
            brightness_save_to_nvs();
        } else {
            volume_save_to_nvs();
        }
    } else {
        s_vol_idle++;
        if (s_vol_state == VOL_STATE_SHOWING &&
            s_vol_idle >= VOL_POP_HIDE_MS / 200) {
            vol_popup_slide_out();
        }
    }
}

/* ── 音量键状态: 上升沿单步 + 长按(>600ms)后每 150ms 重复 ── */
typedef struct {
    bool    prev;
    bool    repeating;
    int64_t next_us;
} vol_key_state_t;
static vol_key_state_t s_vol_up, s_vol_down;

/* 单个音量键状态机: 音乐=音量增减; 小说=亮度增减 (复用同一弹窗) */
static void vol_key_poll_one(int pin, int dir, vol_key_state_t *st)
{
    bool    level = (gpio_get_level(pin) == 1);   /* 高=按下 (外部下拉) */
    int64_t now   = esp_timer_get_time();

    if (level) {
        bool step = false;
        if (!st->prev) {                          /* 上升沿: 立即一次 */
            step = true;
            st->repeating = false;
            st->next_us   = now + VOL_LONGPRESS_MS * 1000LL;
        } else if (!st->repeating) {
            if (now >= st->next_us) {             /* 按住超过 600ms: 进入长按, 补一步 */
                st->repeating = true;
                step = true;
                st->next_us = now + VOL_REPEAT_MS * 1000LL;
            }
        } else if (now >= st->next_us) {          /* 长按中: 每 150ms 一步 */
            step = true;
            st->next_us = now + VOL_REPEAT_MS * 1000LL;
        }

        if (step) {
            if (app_mode_is_novel()) {
                int b = (int)brightness_get() + dir * BRIGHTNESS_STEP;
                if (b < BRIGHTNESS_MIN) b = BRIGHTNESS_MIN;
                if (b > BRIGHTNESS_MAX) b = BRIGHTNESS_MAX;
                brightness_set((uint8_t)b);
                lcd_set_brightness((uint8_t)b);   /* 立即写背光 (之前只改内存值, 屏幕不变) */
                power_mgr_set_cur_bri((uint8_t)b);
            } else {
                volume_inc(dir * VOLUME_STEP);
            }
        }
    } else {
        st->repeating = false;
    }
    st->prev = level;
}

/* 按键轮询 (10ms): 息屏/唤醒键(上升沿) + 音量键增减 */
static void btn_key_poll_cb(lv_timer_t *timer)
{
    (void)timer;
    power_mgr_poll_key(gpio_get_level(PIN_PWR_KEY) == 1);

    vol_key_poll_one(PIN_VOL_UP,   +1, &s_vol_up);
    vol_key_poll_one(PIN_VOL_DOWN, -1, &s_vol_down);
}

/* ── SD 卡状态监视 (20HZ轮询定时器): 插卡加载/拔卡清理 ── */
static void fs_sd_monitor_cb(lv_timer_t *timer)
{
    (void)timer;
    static bool last_ready = false;
    bool sd_ready = atomic_load_bool(&g_sd_ready);

    /* 回收 sys_monitor 移交的旧缓存 (本任务即所有 g_fs_cache 读者) */
    fs_cache_reap();

    if (!sd_ready && last_ready) {   /* 刚拔出 */
        player_on_sd_remove();       /* 停音频/复位播放器/清封面 */
        ui_novel_on_sd_remove();     /* 关小说文件 */

        /* 置空缓存指针: 浏览器/播放器立即 inert */
        g_fs_cache = NULL;
        g_novel_cache = NULL;

        fs_browser_on_sd_remove();   /* 通知浏览器清空 */
        played_bits_on_sd_remove();  /* 释放随机去重位图 */
    }

    if (sd_ready && !last_ready) {   /* 刚插入 */
        fs_browser_on_sd_ready();
        played_bits_on_sd_ready();   /* 全量校验并载入各文件夹随机去重位图 */
        player_on_sd_ready();        /* 音乐模式: 恢复上次歌曲 (仅音乐模式生效) */
        ui_novel_on_sd_ready();      /* 小说模式: 恢复上次打开的小说 (仅小说模式生效) */
    }

    last_ready = sd_ready;
}

void ui_shell_init(void)
{
    /* 关闭屏幕自身滚动条/滚动: 音量/亮度弹窗会滑到屏外 (x=TFT_HOR_RES),
     * 若屏幕可滚动会撑大内容从而在底部冒出水平滚动条. 原先只在音乐 player_build
     * 里做, 直接启动小说模式走不到, 故提到公共初始化 (与模式无关). */
    lv_obj_set_scrollbar_mode(lv_scr_act(), LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(lv_scr_act(), LV_OBJ_FLAG_SCROLLABLE);

    gpio_set_direction(PIN_VOL_UP, GPIO_MODE_INPUT);
    gpio_set_direction(PIN_VOL_DOWN, GPIO_MODE_INPUT);

    vol_popup_create();
    lv_timer_create(btn_key_poll_cb, VOL_KEY_POLL_MS, NULL);
    lv_timer_create(media_monitor_cb, 200, NULL);
    lv_timer_create(fs_sd_monitor_cb, 50, NULL);
}

void ui_shell_on_mode_changed(void)
{
    if (!s_vol_cont) return;
    lv_anim_del(s_vol_cont, NULL);
    lv_obj_add_flag(s_vol_cont, LV_OBJ_FLAG_HIDDEN);
    s_vol_state = VOL_STATE_HIDDEN;
    s_vol_idle = 0;
    vol_popup_sync();   /* 同步为目标模式当前值 (含量程), 避免切换后立刻误弹/残留 */
}
