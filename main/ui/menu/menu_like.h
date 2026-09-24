#ifndef __MENU_LIKE_H__
#define __MENU_LIKE_H__

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── 喜欢/删除 弹窗 (menu_like.c) ──
 * 封面长按触发: 左侧喜欢(心形), 右侧删除; 点背景关闭. */

void like_menu_open(void);      /* 打开弹窗 (需当前有歌曲信息) */
void like_menu_close(void);     /* 关闭并销毁 */
bool like_menu_is_open(void);   /* 是否已打开 */

#ifdef __cplusplus
}
#endif

#endif
