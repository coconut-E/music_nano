#ifndef __UI_LYRICS_H__
#define __UI_LYRICS_H__

#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 歌词页 (挂在音乐组根容器下, 初始隐藏).
 * back_cb: 点击歌词区透明层时回调 (切回封面) */
void lyrics_view_init(lv_obj_t *parent, void (*back_cb)(void));
void lyrics_view_destroy(void);

void lyrics_view_show(void);      /* 显示并恢复 100ms 刷新 */
void lyrics_view_hide(void);      /* 隐藏并暂停刷新 */
void lyrics_view_refresh_now(void);   /* 立即取当前歌曲/时间刷新 (截图前调用) */
bool lyrics_view_has_lyrics(void);

#ifdef __cplusplus
}
#endif

#endif
