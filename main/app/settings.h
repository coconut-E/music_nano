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

/* ── 音量 ──
 * 内部为 32 档 (index 0~31): 显示目标 0,4,...,124; 硬件音量按 8 向上取整 (末档钳到 127);
 * 低于硬件步进的部分由 PCM 软件增益补足 (gain 0~100%).
 * 按钮/耳机按键每次只 ±1 档, 实际值查表. */
#define VOLUME_MIN      0     /* 最小目标音量 (静音, 对应 0 档) */
#define VOLUME_MAX      127   /* 目标音量显示量程上限 (蓝牙 A2DP 标准 0~127) */
#define VOLUME_STEPS    32    /* 内部档位数 */

int32_t volume_index_get(void);                 /* 当前档位 (0~31) */
void    volume_set_index(int32_t idx);          /* 直接设定档位 (内部钳制到 0~31) */
void    volume_inc(int32_t delta);              /* 档位增减 (delta 为档数, 一般为 ±1) */

int32_t volume_get(void);                       /* 当前档位对应目标音量 (0/4/.../124), 供 UI 显示 */
int32_t volume_get_hw(void);                    /* 当前档位对应硬件音量 (8 的倍数, 末档 127), 发蓝牙 */
int32_t volume_get_gain(void);                  /* 当前档位对应软件增益 (%) 0~100 */
void    volume_set_from_hw(int32_t hw);         /* 按耳机回传的绝对音量 (0~127) 映射到最近档位 */

void    volume_load_from_nvs(void);             /* 开机从 NVS 恢复音量档位 */
void    volume_save_to_nvs(void);               /* 音量变化后写回 NVS */

/* ── 亮度 ── */
#define BRIGHTNESS_MIN      1     /* 最小背光亮度 (非 0, 避免全黑) */
#define BRIGHTNESS_MAX      255   /* 最大背光亮度 */
#define BRIGHTNESS_DEFAULT  128   /* 默认亮度 */

uint8_t brightness_get(void);                   /* 读取当前背光亮度 */
void    brightness_set(uint8_t v);              /* 设定背光亮度 (钳制到 1~255) */

void    brightness_load_from_nvs(void);         /* 开机从 NVS 读亮度 */
void    brightness_save_to_nvs(void);           /* 亮度变化后写回 NVS */

/* ── 上次播放歌曲 (含播放进度) ──
 * 同一 NVS 键 "song" 内存 blob: 路径(NUL 结尾) + uint16 千分比进度(0~1000).
 * 进度仅对 >10 分钟的歌按 1 分钟粒度保存, 切歌置零 (策略见 ui_player.c). */
void last_song_save(const char *path);          /* 保存曲目路径到 NVS (进度置零, 切歌用) */
void last_song_progress_save(uint16_t progress);/* 只更新进度 (千分比 0~1000), 路径沿用缓存 */
bool last_song_load(char *buf, size_t size, uint16_t *progress);
                                                /* 读曲目路径与进度, 成功返回 true;
                                                 * buf=路径输出, size=缓冲大小, progress=进度输出(可 NULL) */

/* ── 上次打开的小说 ── */
void last_novel_save(const char *path);         /* 保存上次打开的小说路径到 NVS */
bool last_novel_load(char *buf, size_t size);   /* 读取上次打开的小说, 成功返回 true */

/* ── 小说阅读进度 (按文件名去扩展名的 32 位哈希作 key, 值为文件字节偏移) ── */
void novel_progress_save(const char *path, uint32_t offset);   /* 保存进度 (key 由 path 的文件名哈希得到) */
bool novel_progress_load(const char *path, uint32_t *offset);  /* 读取进度, 成功返回 true; offset=输出偏移 */

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

/* ── 自动休眠 ──
 * 设定值 = 小时 (0~3) + 分钟 (0~60, 步进 10), 但落盘只存一个"总分钟数":
 * 两个下拉各自归一化会存出两个等价状态 (1小时60分 与 2小时0分), 存总分钟数
 * 就只有一个真值, 显示时再拆成 小时=总/60, 分钟=总%60.
 * 小时与分钟同时为 0 没有意义, UI 那边直接禁用保存, 所以可保存区间是 10~240. */
#define AUTOSLEEP_MIN_DEFAULT   60    /* 无记录时的默认时长 (1 小时) */
#define AUTOSLEEP_MIN_MIN       10    /* 最小可保存值 (0 小时 10 分钟) */
#define AUTOSLEEP_MIN_MAX      240    /* 最大可保存值 (3 小时 60 分钟) */

bool settings_sleep_enabled_load(void);         /* 读开关, 无记录返回 false */
void settings_sleep_enabled_save(bool on);      /* 保存开关 */
int  settings_sleep_minutes_load(void);         /* 读设定时长 (分钟), 越界回默认 60 */
void settings_sleep_minutes_save(int total_min);/* 保存设定时长 (钳制 10~240) */

/* ── 自动熄屏 (无操作超时) ──
 * "通用 + 特殊"两层次, 单位分钟 1~10:
 *   通用   = 菜单 / 设置 / 音乐封面, 以及以后新加的任何模式 (默认走这条);
 *   小说   = 特殊覆盖 APP_MODE_NOVEL;
 *   歌词   = 特殊覆盖音乐里的歌词页.
 * 判定顺序写在 power_mgr.c 的 idle_poll_cb 里: 只列举两条特殊分支, 其余一律通用 ——
 * 以后加新模式不用再动那个 if.
 * 注意: 菜单和设置以前是固定 30 秒, 并进通用分支后最短一档是 1 分钟. */
#define SETTINGS_IDLE_COMMON   0   /* 通用分支 */
#define SETTINGS_IDLE_NOVEL    1   /* 特殊: 小说 */
#define SETTINGS_IDLE_LYRICS   2   /* 特殊: 歌词 */
#define SETTINGS_IDLE_NUM      3

#define IDLE_MIN_MIN           1   /* 最短 1 分钟 */
#define IDLE_MIN_MAX           10  /* 最长 10 分钟 */

int  settings_idle_minutes_load(int branch);           /* 读某分支的分钟数, 越界回默认 */
void settings_idle_minutes_save(int branch, int minutes);   /* 保存 (钳制 1~10) */

/* ── 浅睡时长 ──
 * 深睡前先浅睡多久 (浅睡时外设不断电, 按键可立刻唤醒), 0 = 不浅睡、直接深睡.
 * 只影响"短按(蓝牙断开) / 无操作超时 / 息屏期蓝牙断开"三条入口;
 * 自动休眠到点与低电量关机仍然直接深睡. */
#define LIGHT_SLEEP_MIN_MIN      0   /* 0 = 直接深睡 (老行为) */
#define LIGHT_SLEEP_MIN_MAX      10
#define LIGHT_SLEEP_MIN_DEFAULT  5

int  settings_light_sleep_minutes_load(void);           /* 0~10, 越界回默认 5 */
void settings_light_sleep_minutes_save(int minutes);    /* 保存 (钳制 0~10) */

#ifdef __cplusplus
}
#endif

#endif
