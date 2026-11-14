#ifndef __POWER_MGR_H__
#define __POWER_MGR_H__

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 电源管理: GPIO37 息屏/唤醒按键 + 深度睡眠 (GPIO22 切断外设供电) +
 * 电池电压 ADC (开机低电量检测 + sys_monitor 周期采样共用).
 * power_mgr_early_init 在 app_main 第一行调用 (LCD 前半段之前, 开启外设供电);
 * power_mgr_init 在 LVGL 任务中调用; 按键由 vol_key 定时器复用, 10ms 采样调 power_mgr_poll_key.
 * 无操作自动熄屏: 触摸/按键经 power_mgr_notify_activity 续期, 超时后执行与睡眠键短按
 * 相同的动作 (由蓝牙状态自动选择只熄屏或深睡). 时长走"通用 + 特殊"两层次,
 * 设置页可改 (1~10 分钟), 见下面 power_mgr_idle_minutes */
void power_mgr_early_init(void);
void power_mgr_init(void);

/* 开机低电量检测 (app_main 在 lcd_init_early 之后调用):
 * 初始化 ADC + 3 次采样平均 → 写入 g_vbat.
 * 电压正常直接返回; 电压过低则显示低电量图 2 秒后进入深度睡眠 (不返回) */
void power_mgr_boot_battery_check(void);

/* 单次读取电池电压 (V): 供 sys_monitor 周期采样等复用.
 * 首次调用会幂等初始化 ADC. 返回负值表示 ADC 不可用/读取失败,
 * 调用方必须先判 v>=0 再参与低压判断, 避免误判 0V */
float power_mgr_vbat_read_once(void);

/* 紧急关机 (运行中电压过低时调用, 无动画):
 * 背光直接拉低 → GPIO22 切断外设供电 → GPIO37 高电平唤醒 → 深度睡眠 (不返回).
 * 供 sys_monitor 周期采样检测到低压时直接关机 */
void power_mgr_critical_shutdown(void);

/* 外部亮度变化 (亮度滑块) 时同步当前亮度, 保证淡入/淡出起点正确.
 * v=新的当前亮度 (0~255) */
void power_mgr_set_cur_bri(uint8_t v);

/* 记录一次用户操作 (触摸/按键), 重置无操作熄屏计时.
 * 触摸层在每次读到触点时调用 (含空容器/无效区), 按键在按下沿/步进时调用.
 * 仅可在 LVGL 任务上下文调用 (与内部 idle 定时器同任务) */
void power_mgr_notify_activity(void);

/* 睡眠抑制开关: OTA 新固件校验窗口期间置位.
 * 置位后电源键短按/长按与无操作熄屏全部失效 (触摸/音量键不受影响),
 * 目的是防止用户在"请勿断电"窗口里误触导致新固件来不及打标就被回退 */
void power_mgr_set_sleep_inhibit(bool inhibit);
bool power_mgr_sleep_inhibited(void);

/* 界面抑制开关 (设置页 WiFi 流程): 置位期间电源键短按/长按与无操作熄屏全部失效,
 * 触摸/音量键不受影响. 与 power_mgr_set_sleep_inhibit 各存一个标志、互不覆盖
 * (OTA 校验窗口可能同时在用). 扫描/连接/释放期间的"按了没反应"就是它实现的 */
void power_mgr_set_ui_inhibit(bool inhibit);

/* 设置歌词页是否显示: true=歌词页 (走"歌词"这条特殊熄屏分支), false=音乐封面
 * (走"通用"分支). 由音乐 UI 在进入/退出歌词页、销毁音乐组时调用 */
void power_mgr_set_lyrics_active(bool active);

/* ── 自动熄屏 (无操作超时) ──
 * "通用 + 特殊"两层次, 单位分钟 1~10:
 *   通用 = 菜单/设置/音乐封面 以及以后新加的任何模式; 小说、歌词是两条特殊覆盖.
 * branch 取值与 settings.h 的 SETTINGS_IDLE_* 一致.
 * 读的是开机灌入的缓存 (1 秒轮询每次都用), 改设置走 power_mgr_idle_set: 立刻生效. */
int  power_mgr_idle_minutes(int branch);
void power_mgr_idle_set(int branch, int minutes);

/* 定时休眠到点 (autosleep): 不判断蓝牙状态, 背光淡出后直接深睡 —— 到点就是关机,
 * 音乐随之结束. 与电源键短按共用同一条淡出/深睡路径, 并遵守同样的抑制规则
 * (OTA 校验窗口 / WiFi 流程期间不睡). 由 autosleep 的 1 秒定时器在 LVGL 任务里调 */
void power_mgr_sleep_now(void);

/* ── 浅睡 (light sleep) ──
 * 电源键短按(蓝牙断开) / 无操作超时 / 息屏期蓝牙断开: 背光正常淡出后先浅睡 N 分钟
 * (外设不断电、程序从断点继续), 期间按电源键立刻淡入亮屏; 到点转深睡.
 * N 由设置页「浅睡时长」给 (0~10 分钟), **0 = 不浅睡, 直接深睡** (老行为).
 * 蓝牙已连接时仍然只熄屏, 不睡.
 * 下面这个只是诊断入口: 浅睡 sec 秒并自检 PSRAM/内部 RAM/屏幕/触摸/SD, 正式接入后
 * 可以删掉. */
void power_mgr_light_sleep_test(int sec);

/* 浅睡时长 (分钟): 读缓存 / 改设置 (缓存 + NVS, 立刻生效) */
int  power_mgr_light_sleep_minutes(void);
void power_mgr_light_sleep_set_minutes(int minutes);

/* 按键电平上报 (调用方每 10ms 上报一次当前电平).
 * 短按(松手时): 息屏/亮屏/浅睡 (见上); 长按(>=800ms): 进入主菜单
 * (已在菜单里则无反应). level=按键当前电平 (true=按下) */
void power_mgr_poll_key(bool level);

/* 模式切换黑屏过渡: 淡出到纯黑 → 调用 work() (清理+构建) → 淡入.
 * work 在 LVGL 任务上下文执行; 过渡期间屏蔽按键与蓝牙轮询.
 * backlight_fade=true 时用背光淡出/淡入代替像素过渡 (过渡期间不写任何像素,
 * 彻底没有分带接缝; 设置页大白底用它). false 走光圈缩放动画. */
void power_mgr_mode_transition(void (*work)(void), bool backlight_fade);

#ifdef __cplusplus
}
#endif

#endif
