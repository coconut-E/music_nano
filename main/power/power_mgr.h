#ifndef __POWER_MGR_H__
#define __POWER_MGR_H__

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 电源管理: 电源键 (GPIO37) + 三级睡眠梯 + 无操作自动熄屏.
 * 电池电压检测在 power_batt.h, 背光淡化在 power_fade.h.
 *
 * 睡眠不是三个并列的分支, 而是同一件事的三个档位 —— 只能顺着往下走:
 *
 *     亮屏        背光正常, 触摸/刷新正常
 *       │  一级 pwr_off()      背光淡出到 0, 触摸关, LVGL 刷新降到 100s
 *       ▼
 *     灭屏        蓝牙/音频照常; 按键/触摸立刻淡入回来
 *       │  二级 pwr_down()     淡出 → 浅睡 → 深睡
 *       ▼
 *     浅睡        CPU/AHB/APB 停钟, 外设一直供电; 电源键唤醒后从断点继续
 *       │  定时到点 / 未启用浅睡
 *       ▼
 *     深睡        GPIO22 断电, ext0 唤醒, 不返回
 *
 * 所有"想睡/想醒"的事件都只有**一个入口** power_mgr_sleep_event(): 先查抑制,
 * 再把 (事件, 当前状态, 蓝牙状态, 设置) 过一遍决策表, 最后交给唯一的执行器.
 * 加一种触发方式 = 加一个事件; 加一个档位 = 加一个动作; 不要在事件源里写 if. */

void power_mgr_early_init(void);
void power_mgr_init(void);

/* 睡眠 / 熄屏的触发事件. 只有这四种能让设备睡下或醒来 */
typedef enum {
    PWR_EV_KEY = 0,   /* 电源键短按 (长按进菜单属于按键语义, 不算睡眠事件) */
    PWR_EV_IDLE,      /* 无操作超时: 替用户按一下电源键 */
    PWR_EV_BT_LOST,   /* 灭屏期间蓝牙断开: 音频没了, 没必要继续亮着等 */
    PWR_EV_TIMER,     /* 自动休眠到点 (不看蓝牙, 到点就关机) */
} pwr_ev_t;

/* 统一睡眠入口: 决策 + 执行. 只能在 LVGL 任务上下文调用 (和内部定时器同任务) */
void power_mgr_sleep_event(pwr_ev_t ev);

/* 立即关机 (无动画): GPIO22 断电 → ext0 唤醒 → 深睡, 不返回.
 * 只给"必须马上断电"的场景用 (低电压 / 开机低压); 正常睡眠走
 * power_mgr_sleep_event —— 那条路才有淡出、浅睡和抑制判断 */
void power_mgr_shutdown_now(void);

/* 记录一次用户操作 (触摸/按键), 重置无操作熄屏计时.
 * 触摸层在每次读到触点时调用 (含空容器/无效区), 按键在按下沿/步进时调用.
 * 仅可在 LVGL 任务上下文调用 (与内部 idle 定时器同任务) */
void power_mgr_notify_activity(void);

/* 睡眠抑制开关: OTA 新固件校验窗口期间置位.
 * 置位后电源键短按/长按与所有自动睡眠全部失效 (触摸/音量键不受影响),
 * 目的是防止用户在"请勿断电"窗口里误触导致新固件来不及打标就被回退 */
void power_mgr_set_sleep_inhibit(bool inhibit);
bool power_mgr_sleep_inhibited(void);

/* 界面抑制开关 (设置页 WiFi 流程): 置位期间电源键短按/长按与自动睡眠全部失效,
 * 触摸/音量键不受影响. 与 power_mgr_set_sleep_inhibit 各存一个标志、互不覆盖
 * (OTA 校验窗口可能同时在用). 扫描/连接/释放期间的"按了没反应"就是它实现的 */
void power_mgr_set_ui_inhibit(bool inhibit);

/* 设置歌词页是否显示: true=歌词页, false=音乐封面. 由音乐 UI 在进入/退出歌词页、
 * 销毁音乐组时调用. 只影响"无操作超时用哪个分支的时长" (见下面 SETTINGS_IDLE_*) */
void power_mgr_set_lyrics_active(bool active);

/* ── 自动熄屏 (无操作超时) ──
 * "通用 + 特殊"两层次, 单位分钟 1~10:
 *   通用 = 菜单/设置/音乐封面 以及以后新加的任何模式; 小说、歌词是两条特殊覆盖.
 * branch 取值与 settings.h 的 SETTINGS_IDLE_* 一致.
 * 读的是开机灌入的缓存 (1 秒轮询每次都用), 改设置走 power_mgr_idle_set: 立刻生效. */
int  power_mgr_idle_minutes(int branch);
void power_mgr_idle_set(int branch, int minutes);

/* ── 浅睡时长 ──
 * 电源键短按(蓝牙断开) / 无操作超时 / 息屏期蓝牙断开: 一级灭屏后先浅睡 N 分钟,
 * 期间按电源键立刻淡入亮屏; 到点转深睡. N 由设置页「浅睡时长」给 (0~10 分钟),
 * **0 = 不浅睡, 直接深睡** (老行为). 自动休眠到点不走这一级 (到点就是关机). */
int  power_mgr_light_sleep_minutes(void);
void power_mgr_light_sleep_set_minutes(int minutes);

/* 按键电平上报 (调用方每 10ms 上报一次当前电平).
 * 短按(松手时): 报 PWR_EV_KEY; 长按(>=800ms): 进入主菜单 (已在菜单里则无反应).
 * level=按键当前电平 (true=按下) */
void power_mgr_poll_key(bool level);

/* 模式切换黑屏过渡: 淡出到纯黑 → 调用 work() (清理+构建) → 淡入.
 * work 在 LVGL 任务上下文执行; 过渡期间屏蔽按键与所有睡眠事件.
 * backlight_fade=true 时用背光淡出/淡入代替像素过渡 (过渡期间不写任何像素,
 * 彻底没有分带接缝; 设置页大白底用它). false 走光圈缩放动画. */
void power_mgr_mode_transition(void (*work)(void), bool backlight_fade);

#ifdef __cplusplus
}
#endif

#endif
