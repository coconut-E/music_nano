#ifndef __POWER_FADE_H__
#define __POWER_FADE_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 背光淡入 / 淡出: 全工程唯一改"当前亮度"的地方.
 *
 * 为什么不用 lv_anim: 它的定时器跟着 LVGL 刷新周期走 (20ms 一跳), 200ms 的过渡
 * 只有 10 步, 白底页面上每步约 10% 亮度, 看上去就是一档一档往上跳. 这里自己开
 * 6ms 的定时器, 同样的总时长走 30+ 步, 才是连续变暗. 步长只由"驱动频率 / 总时长"
 * 决定, 和亮度区间怎么切没关系 —— 所以分段淡化不能让步长变短, 只会把分带接缝
 * (那根灰线) 重新引进来.
 *
 * 起点必须准: 滑块/按键直接改了亮度后要调 pwr_fade_set 同步, 否则下一次淡出会从
 * 旧值跳一下. 屏幕是否"已经全黑"也只有这里知道 (pwr_fade_is_dark), 睡眠路径靠它
 * 判断"还要不要淡出" */

/* 同步当前亮度 (外部直接改了背光之后调用) */
void pwr_fade_set(int v);

/* 当前亮度 (0~255) */
int  pwr_fade_now(void);

/* 当前是否已经全黑 (== 0) */
bool pwr_fade_is_dark(void);

/* 从当前亮度淡到 target, 时长按亮度差映射 50~400ms; done 可为 NULL.
 * 重复调用会顶掉上一次, 不会有两条动画抢着写亮度 */
void pwr_fade_to(int target, void (*done)(void));

/* 淡到 target, 固定时长 (模式切换的黑场用, 150/180ms 与光圈动画对齐) */
void pwr_fade_to_ms(int target, uint32_t ms, void (*done)(void));

#ifdef __cplusplus
}
#endif

#endif
