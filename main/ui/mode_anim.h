#ifndef __MODE_ANIM_H__
#define __MODE_ANIM_H__

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 模式切换"光圈缩放"过渡动画 (截图合成).
 * 流程: 快照旧界面 → 缩小+圆角+淡出 → 全黑 → switch_work() 切换 UI
 *       → 快照新界面 → 放大+去圆角+淡入 → done().
 *
 * switch_work: 黑场窗口内执行的切换工作 (LVGL 任务上下文, 同步调用一次)
 * done:        动画全部结束后的回调 (恢复触摸等); 可为 NULL
 * 返回 true=动画已启动 (异步完成); false=资源不足, 调用方应走降级路径
 *
 * 需在 LVGL 任务中调用. */
bool mode_anim_run(void (*switch_work)(void), void (*done)(void));

#ifdef __cplusplus
}
#endif

#endif
