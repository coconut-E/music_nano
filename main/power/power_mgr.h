#ifndef __POWER_MGR_H__
#define __POWER_MGR_H__

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 电源管理: GPIO37 息屏/唤醒按键 + 深度睡眠 (GPIO22 切断外设供电) +
 * 电池电压 ADC (开机低电量检测 + sys_monitor 周期采样共用).
 * power_mgr_early_init 在 app_main 第一行调用 (LCD 前半段之前, 开启外设供电);
 * power_mgr_init 在 LVGL 任务中调用; 按键由 vol_key 定时器复用, 10ms 采样调 power_mgr_poll_key */
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

/* 按键电平上报 (调用方每 10ms 上报一次当前电平).
 * 短按(松手时): 息屏/亮屏或深睡 (原逻辑); 长按(>=800ms): 切换音乐/小说模式.
 * level=按键当前电平 (true=按下) */
void power_mgr_poll_key(bool level);

/* 模式切换黑屏过渡: 淡出到纯黑 → 调用 work() (清理+构建) → 淡入.
 * work 在 LVGL 任务上下文执行; 过渡期间屏蔽按键与蓝牙轮询 */
void power_mgr_mode_transition(void (*work)(void));

#ifdef __cplusplus
}
#endif

#endif
