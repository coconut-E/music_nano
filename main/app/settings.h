#ifndef __SETTINGS_H__
#define __SETTINGS_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 设置持久化 (NVS, 统一 "player" 命名空间):
 * 音量 / 上次播放歌曲 / 播放模式
 */

/* ── 音量 ── */
#define VOLUME_MIN      0     /* 最小音量 (静音) */
#define VOLUME_MAX      127   /* 最大音量 (蓝牙 A2DP 标准 0~127) */

int32_t volume_get(void);                       /* 读取当前音量 */
void    volume_set(int32_t v);                  /* 设定音量 (内部钳制到 0~127, 并同步到蓝牙) */
void    volume_inc(int32_t delta);              /* 音量增减 (delta 可为负) */

void    volume_load_from_nvs(void);             /* 开机从 NVS 读音量 */
void    volume_save_to_nvs(void);               /* 音量变化后写回 NVS */

/* ── 亮度 ── */
#define BRIGHTNESS_MIN      1     /* 最小背光亮度 (非 0, 避免全黑) */
#define BRIGHTNESS_MAX      255   /* 最大背光亮度 */
#define BRIGHTNESS_DEFAULT  128   /* 默认亮度 */

uint8_t brightness_get(void);                   /* 读取当前背光亮度 */
void    brightness_set(uint8_t v);              /* 设定背光亮度 (钳制到 1~255) */

void    brightness_load_from_nvs(void);         /* 开机从 NVS 读亮度 */
void    brightness_save_to_nvs(void);           /* 亮度变化后写回 NVS */

/* ── 上次播放歌曲 ── */
void last_song_save(const char *path);          /* 保存上次播放曲目路径到 NVS */
bool last_song_load(char *buf, size_t size);    /* 读取上次播放曲目, 成功返回 true; buf=输出缓冲, size=缓冲大小 */

/* ── 播放模式 ── */
typedef enum {
    PLAY_MODE_SEQUENTIAL = 0,   /* 顺序(到头回绕) */
    PLAY_MODE_SINGLE,           /* 单曲循环 */
    PLAY_MODE_RANDOM,           /* 随机 */
} play_mode_t;

bool settings_mode_load(play_mode_t *mode);     /* 从 NVS 读播放模式, 成功返回 true; mode=输出 */
void settings_mode_save(play_mode_t mode);      /* 保存播放模式到 NVS */

/* ── 循环次数 (随机/顺序: 每首重复次数, 1~9) ── */
#define LOOP_COUNT_MIN  1
#define LOOP_COUNT_MAX  9

int  settings_loop_count_load(void);            /* 读循环次数, 无记录/越界返回 1 */
void settings_loop_count_save(int n);           /* 保存循环次数到 NVS (钳制 1~9) */

/* ── 应用模式 (音乐 / 小说): 取值约定与 app_mode.h 的 app_mode_t 一致 ── */
#define SETTINGS_APP_MODE_MUSIC  0   /* 音乐模式 */
#define SETTINGS_APP_MODE_NOVEL  1   /* 小说阅读模式 */

int  settings_app_mode_load(void);              /* 读上次模式, 无记录/越界返回 0 (音乐) */
void settings_app_mode_save(int mode);          /* 保存当前模式到 NVS */

#ifdef __cplusplus
}
#endif

#endif
