#ifndef __UI_SHELL_H__
#define __UI_SHELL_H__

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 通用 UI 运行时 (常驻, 不随音乐/小说模式销毁):
 *  - 音量/亮度共用弹窗 (音乐=音量, 小说=亮度)
 *  - 音量键 / 睡眠键轮询 (音量键按当前模式分派到音量或亮度)
 *  - SD 卡就绪/拔出监视与分发
 * 在 UI 任务启动时调用一次 ui_shell_init()。 */
void ui_shell_init(void);

/* 模式切换后调用: 复位弹窗量程/状态并隐藏弹窗 */
void ui_shell_on_mode_changed(void);

/* 音量/亮度弹窗当前是否可见 (截图合成动画用来决定是否保留右侧弹窗) */
bool ui_shell_volume_visible(void);

#ifdef __cplusplus
}
#endif

#endif
