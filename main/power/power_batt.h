#ifndef __POWER_BATT_H__
#define __POWER_BATT_H__

#ifdef __cplusplus
extern "C" {
#endif

/* 电池电压 (GPIO ADC): 开机低压检测 + sys_monitor 周期采样共用一套 ADC 初始化.
 *
 * 和睡眠路径的关系只有一条: 电压过低时调 power_mgr_shutdown_now() 立刻断电深睡,
 * 不走决策表 —— 低电量没有任何可选项 (不能只熄屏, 也不能浅睡). */

/* 单次读取电池电压 (V): 供 sys_monitor 周期采样用. 首次调用幂等初始化 ADC.
 * 返回负值表示 ADC 不可用/读取失败, 调用方必须先判 v>=0 再参与低压判断,
 * 避免把 0V 当成真电压误关机 */
float power_batt_read_once(void);

/* 开机低电量检测 (app_main 在 lcd_init_early 之后调用):
 * 初始化 ADC + 3 次采样平均 → 写入 g_vbat.
 * 电压正常直接返回; 电压过低则显示低电量图 2 秒后深睡 (不返回) */
void power_batt_boot_check(void);

/* 运行中电压过低 (sys_monitor 调用): 无动画直接深睡 (不返回) */
void power_batt_critical_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
