#ifndef __MENU_MODE_H__
#define __MENU_MODE_H__

#ifdef __cplusplus
extern "C" {
#endif

/* ── 循环次数设置弹窗 (menu_mode.c) ──
 * 长按播放模式按钮触发: 从底部滑入, 设置随机/顺序每首重复次数 (1~9). */
void mode_menu_open(void);

#ifdef __cplusplus
}
#endif

#endif
