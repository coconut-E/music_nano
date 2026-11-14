#ifndef __AUTOSLEEP_H__
#define __AUTOSLEEP_H__

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── 自动休眠 (音乐模式专用) ──
 *
 * 分工: 开关与设定时长存 NVS (settings.c 的 sleep_on / sleep_min);
 * 剩余时间是纯内存的临时量, 重启就重来 —— 用户要求"重启了不用管它".
 *
 * 计时规则:
 *   - 只在音乐模式下走 (app_mode_get() == APP_MODE_MUSIC);
 *   - 进菜单/设置/小说时暂停, 剩余量保留; 回音乐接着走, 不补扣暂停那段;
 *   - 息屏不算离开音乐 (蓝牙还在放), 计时继续;
 *   - 到点直接深睡 (切断外设供电、音乐结束), 不是"只熄屏".
 *
 * 线程: init 与 set_xxx 与 format 都在 LVGL 任务里调用; 串口测试用 request_sec
 * 只写一个 int 标志, 由 1 秒定时器消费 (剩余量是 int64, 跨任务直接写会撕裂). */

void autosleep_init(void);                  /* LVGL 任务: 读 NVS + 建 1 秒定时器 */

bool autosleep_is_on(void);                 /* 开关当前状态 */
int  autosleep_total_minutes(void);         /* 设定时长 10~240 分钟 */
int  autosleep_remaining_sec(void);         /* 剩余秒数 (关闭时 0), 供串口/UI 看 */

void autosleep_set_on(bool on);             /* 开: 按设定从头计时; 关: 剩余清零 (+写 NVS) */
void autosleep_set_minutes(int total_min);  /* 写 NVS; 开着则按新时长重新计时 */

/* 左侧文案: 开 → "将在1小时30分钟后休眠" (按剩余向上取整到分钟); 关 → "自动休眠" */
void autosleep_format_left(char *buf, size_t cap);

/* 串口测试: 把剩余时间设为 sec 秒并立即开始计时 (消费发生在下一次 1 秒定时器) */
void autosleep_request_sec(int sec);

/* 浅睡醒来时调用: 只把结算基准拉到现在, 让浅睡那几分钟不从剩余量里扣.
 * (esp_timer 会把睡着的时间补上来, 不重置的话醒来第一拍就剩余归零 → 刚亮屏又被关机) */
void autosleep_on_wake(void);

#ifdef __cplusplus
}
#endif

#endif
