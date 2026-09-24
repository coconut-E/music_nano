#ifndef __UI_PLAYER_H__
#define __UI_PLAYER_H__

#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 创建播放器主界面 (ui_loop_task 内调用) */
void ui_player_init(void);

/* 播放控制: 文件浏览器选中 / 自动切歌 / 响应音频结果.
 * player_play_file: 播放指定文件 (group=分组, name=文件名) */
void player_play_file(const char *group, const char *name);
void player_advance(void);               /* 按播放模式切到下一首 */
void player_on_song_finished(void);      /* 音频通知一首播完 */
void player_on_file_not_found(void);     /* 音频通知文件不存在 */

/* 播放/暂停切换 (播放键 + 蓝牙耳机请求共用) */
void player_toggle_play(void);
void player_next(void);                  /* 下一首 */
void player_prev(void);                  /* 上一首 */

/* 封面: 解码任务就绪后回调. buf=100x100 RGB565 封面或 NULL(无封面) */
void player_show_cover(void *buf);

/* 播放影子状态 (供主循环分发使用) */
bool player_was_playing(void);           /* 是否曾处于播放 (用于蓝牙重连续播) */
void player_set_was_playing(bool v);

/* 当前播放信息: 供文件浏览器定位正在播放的歌曲 (无歌曲时返回 NULL) */
const char *player_current_group(void);  /* 当前分组 */
const char *player_current_name(void);   /* 当前文件名 */

/* 循环次数 (随机/顺序: 每首重复次数, 1~9) */
int  player_get_loop_count(void);
void player_set_loop_count(int n);       /* 钳制 1~9, 并写 NVS */

/* 删除当前播放文件: 关弹框后调用. 停音频→等其释放 SD→删除→修位图→重扫 (异步, 不阻塞 UI) */
bool player_request_delete_current_file(void);

#ifdef __cplusplus
}
#endif

#endif
