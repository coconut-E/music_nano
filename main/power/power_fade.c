/* 背光淡化: 6ms 一步的线性过渡, 全局只有这一份实现.
 *
 * 步进定时器用的是 lv_timer (和 LVGL 同任务), 但它和"刷新定时器"是两回事 ——
 * 息屏时刷新周期被拉到 100s, 这里的淡化照样按 6ms 走, 因为亮度是直接写 LEDC,
 * 不需要重绘任何像素. */
#include <stdint.h>
#include "esp_timer.h"
#include "lvgl.h"
#include "drv_display.h"
#include "power_fade.h"

/* 淡出/淡入: 变化幅度 1~255 → 总时长 50~400ms (线性映射, 避免拉满后过长) */
#define BRI_FADE_TIME_MIN_MS  50
#define BRI_FADE_TIME_MAX_MS  400

/* 每步间隔: 200ms 的过渡走 33 步, 每步约 3% 亮度 —— 白底上肉眼已看不出台阶 */
#define BRI_STEP_MS           6

static int      s_cur_bri = 0;               /* 当前实际背光亮度 */
static lv_timer_t *s_timer = NULL;           /* 淡化驱动定时器 */
static int      s_from    = 0;               /* 起始亮度 */
static int      s_to      = 0;               /* 目标亮度 */
static uint32_t s_ms      = 1;               /* 总时长 */
static int64_t  s_t0      = 0;               /* 起始时刻 (us) */
static void   (*s_done)(void) = NULL;        /* 完成回调 */

/* 变化幅度 → 总时长: diff 越大越慢, 但夹在 50~400ms (diff=0 不淡) */
static int fade_time_ms(int diff)
{
    if (diff <= 0) return 1;
    if (diff > 255) diff = 255;
    return BRI_FADE_TIME_MIN_MS + (diff - 1) * (BRI_FADE_TIME_MAX_MS - BRI_FADE_TIME_MIN_MS) / (255 - 1);
}

void pwr_fade_set(int v)
{
    if (v < 0)   v = 0;
    if (v > 255) v = 255;
    s_cur_bri = v;
}

int pwr_fade_now(void)
{
    return s_cur_bri;
}

bool pwr_fade_is_dark(void)
{
    return s_cur_bri == 0;
}

/* 每 6ms 写一次背光: 用"已经过去多久 / 总时长"算位置, 不依赖调用次数,
 * 所以中途被别的定时器挤一下就慢一帧, 总时长仍然准确 */
static void step_cb(lv_timer_t *tmr)
{
    int el = (int)((esp_timer_get_time() - s_t0) / 1000);   /* ms */
    int p  = (s_ms > 0) ? (int)((int64_t)el * 1000 / s_ms) : 1000;
    if (p > 1000) p = 1000;

    int v = s_from + (s_to - s_from) * p / 1000;
    if (v != s_cur_bri) {
        s_cur_bri = v;
        lcd_set_brightness(v);
    }

    if (p < 1000) return;

    lv_timer_del(tmr);
    s_timer = NULL;
    void (*done)(void) = s_done;      /* 先摘下来再调: 回调里可能又起一段淡化 */
    s_done = NULL;
    if (done) done();
}

static void fade_start(int target, uint32_t ms, void (*done)(void))
{
    if (s_timer) { lv_timer_del(s_timer); s_timer = NULL; }
    s_from = s_cur_bri;
    s_to   = target;
    s_ms   = ms ? ms : 1;
    s_t0   = esp_timer_get_time();
    s_done = done;
    s_timer = lv_timer_create(step_cb, BRI_STEP_MS, NULL);
}

void pwr_fade_to(int target, void (*done)(void))
{
    int diff = target - s_cur_bri;
    if (diff < 0) diff = -diff;
    fade_start(target, (uint32_t)fade_time_ms(diff), done);
}

void pwr_fade_to_ms(int target, uint32_t ms, void (*done)(void))
{
    fade_start(target, ms, done);
}
