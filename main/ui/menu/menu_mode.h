#ifndef __MENU_MODE_H__
#define __MENU_MODE_H__

#ifdef __cplusplus
extern "C" {
#endif

/* ── 播放模式弹窗 (menu_mode.c) ──
 * 长按播放模式按钮触发: 从底部滑入, 顶部是「自动休眠」开关 (含剩余时间文案),
 * 下面是随机/顺序每首重复次数 (1~9). */
void mode_menu_open(void);

#ifdef __cplusplus
}
#endif

#endif
