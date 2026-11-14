/* 自动休眠的运行时: 剩余时间、1 秒结算、到点请求深睡.
 *
 * 为什么存"剩余量"而不是"到点时刻":
 *   计时只在音乐模式下走, 进一次主菜单再回来, 绝对时刻会被暂停期间的时间一口
 *   气扣掉; 存剩余量 + 每秒结算天然就是"暂停/继续"语义, 不需要额外的暂停标志.
 *
 * 为什么不在 power_mgr.c 里做:
 *   那边管的是"什么时候该睡" (按键/无操作/低电量), 这里是"还剩多久", 失败模式
 *   也完全不同 (一个是定时器算错, 一个是 GPIO/深睡时序), 分开放各自能单独看懂.
 *
 * 时间基准用 esp_timer_get_time() 而不是 lv_tick: LVGL 的 tick 在息屏降频
 * (power_mgr 把刷新周期拉到 100 秒) 时仍然走, 但 esp_timer 是独立硬件计时器,
 * 不受 UI 负载影响. */
#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "app_mode.h"
#include "power_mgr.h"
#include "settings.h"
#include "autosleep.h"

static const char *TAG = "ASLEEP";

#define TICK_MS     1000                     /* 结算周期 */
#define US_PER_S    1000000LL
#define US_PER_MIN  60LL * US_PER_S

static bool    s_on        = false;          /* 开关 (与 NVS 同源) */
static int     s_total_min = AUTOSLEEP_MIN_DEFAULT;   /* 设定时长 */
static int64_t s_left_us   = 0;              /* 剩余 (临时量, 不入 NVS) */
static int64_t s_tick_us   = 0;              /* 上次结算时刻 */
static bool    s_firing    = false;          /* 已请求深睡: 淡出期间不要重复触发 */

/* 给其它任务看的整秒快照 (32 位读写天然原子, 不必为一次串口打印上锁) */
static volatile int s_left_sec = 0;

/* 串口请求的测试时长: console 任务写, 1 秒定时器读走 */
static volatile int s_req_sec = 0;

/* 按 minutes 从头开始计时 (0 = 只清零剩余) */
static void timer_start(int minutes)
{
    s_left_us  = (int64_t)minutes * US_PER_MIN;
    s_tick_us  = esp_timer_get_time();
    s_firing   = false;
    s_left_sec = minutes * 60;
}

/* 1 秒结算: 只有"开关开 + 在音乐模式"才扣时间, 其它情况只跟住时钟 */
static void tick_cb(lv_timer_t *tmr)
{
    (void)tmr;

    int64_t now = esp_timer_get_time();
    int64_t d   = now - s_tick_us;
    s_tick_us   = now;                        /* 暂停期间也要更新, 否则回到音乐时补扣一大段 */

    int req = s_req_sec;                      /* 测试请求 (见 autosleep_request_sec) */
    if (req > 0) {
        s_req_sec = 0;
        if (!s_on) {                          /* 关着就直接开, 并落盘, 行为与点开关一致 */
            s_on = true;
            settings_sleep_enabled_save(true);
        }
        timer_start(0);
        s_left_us  = (int64_t)req * US_PER_S;
        s_left_sec = req;
        ESP_LOGI(TAG, "测试: 剩余 %d 秒后深睡", req);
    }

    if (!s_on || s_firing) {
        s_left_sec = 0;
        return;
    }

    if (app_mode_get() != APP_MODE_MUSIC) {   /* 离开音乐: 暂停, 剩余量保留 */
        s_left_sec = (int)(s_left_us / US_PER_S);
        return;
    }

    if (d > 0) s_left_us -= d;
    if (s_left_us <= 0) {                     /* 到点: 直接深睡 */
        s_left_us  = 0;
        s_left_sec = 0;
        s_firing   = true;
        ESP_LOGI(TAG, "定时到点, 进入休眠");
        power_mgr_sleep_now();
        return;
    }
    s_left_sec = (int)(s_left_us / US_PER_S);
}

void autosleep_init(void)
{
    s_on        = settings_sleep_enabled_load();
    s_total_min = settings_sleep_minutes_load();

    if (s_on) {
        /* 开机时开关就是开的: 从开机这一刻开始计 (用户要求"启动时开着也自动休眠") */
        timer_start(s_total_min);
        ESP_LOGI(TAG, "开机即开启, 设定 %d 分钟", s_total_min);
    } else {
        timer_start(0);
    }

    lv_timer_create(tick_cb, TICK_MS, NULL);
}

bool autosleep_is_on(void)
{
    return s_on;
}

int autosleep_total_minutes(void)
{
    return s_total_min;
}

int autosleep_remaining_sec(void)
{
    return s_left_sec;
}

void autosleep_set_on(bool on)
{
    if (on == s_on) return;                   /* 状态没变不动剩余量 (避免重复重置) */

    s_on = on;
    settings_sleep_enabled_save(on);

    if (on) {
        timer_start(s_total_min);             /* 从这个开关打开的时刻起算 */
        ESP_LOGI(TAG, "开启, %d 分钟后休眠", s_total_min);
    } else {
        timer_start(0);                       /* 关闭: 剩余量清零 */
        ESP_LOGI(TAG, "关闭, 剩余清零");
    }
}

void autosleep_set_minutes(int total_min)
{
    if (total_min < AUTOSLEEP_MIN_MIN) total_min = AUTOSLEEP_MIN_MIN;
    if (total_min > AUTOSLEEP_MIN_MAX) total_min = AUTOSLEEP_MIN_MAX;

    s_total_min = total_min;
    settings_sleep_minutes_save(total_min);

    if (s_on) {                               /* 开着改了时长: 按新时长重新计时 */
        timer_start(total_min);
        ESP_LOGI(TAG, "时长改为 %d 分钟, 重新计时", total_min);
    }
}

void autosleep_format_left(char *buf, size_t cap)
{
    if (!buf || cap == 0) return;

    if (!s_on) {
        snprintf(buf, cap, "自动休眠");
        return;
    }

    /* 向上取整到分钟: 还剩 29 秒时显示"1分钟"而不是"0分钟"; 取整到分钟而不是
     * 直接除, 是为了让 1 小时 60 分这种边界自然进位成 2 小时 0 分 */
    int64_t rem_s   = (s_left_us + US_PER_S - 1) / US_PER_S;
    int     tot_min = (int)((rem_s + 59) / 60);
    int     h       = tot_min / 60;
    int     m       = tot_min % 60;

    if (h > 0) snprintf(buf, cap, "将在%d小时%d分钟后休眠", h, m);
    else       snprintf(buf, cap, "将在%d分钟后休眠", m);
}

void autosleep_request_sec(int sec)
{
    if (sec <= 0) return;
    if (sec > 24 * 3600) sec = 24 * 3600;
    s_req_sec = sec;
}

/* 浅睡醒来: 睡眠期间 1 秒定时器是停的, 但 esp_timer 前进了整段时间.
 * 这里把基准挪到"现在", 于是那段时间不扣剩余量 —— 浅睡本身就是在睡, 不该再算。 */
void autosleep_on_wake(void)
{
    s_tick_us = esp_timer_get_time();
}
