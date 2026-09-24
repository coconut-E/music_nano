#ifndef __AUDIO_H__
#define __AUDIO_H__

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ──────────────────────────── 歌曲信息 ────────────────────────────
 * 解码任务写入, LVGL 只读。
 * 写入顺序: 先填充各字段, 最后置 g_song_info_valid = true;
 * 读取顺序: 先读 g_song_info_valid, 为 true 后再读 g_song_info。
 */
#define SONG_TITLE_MAX   128   /* 歌名最大长度 */
#define SONG_ARTIST_MAX  128   /* 歌手最大长度 */
#define SONG_FORMAT_MAX  16    /* 格式名最大长度 */

/* 当前歌曲信息 (解码任务写入, UI 只读) */
typedef struct {
    char     title[SONG_TITLE_MAX];    /* 歌名 */
    char     artist[SONG_ARTIST_MAX];  /* 歌手 */
    char     format[SONG_FORMAT_MAX];  /* 编码格式 (MP3/FLAC/WAV/AAC, 仅按扩展名判定) */
    uint32_t sample_rate;              /* 采样率 (Hz) */
    uint8_t  channels;                 /* 声道数 */
    uint8_t  bits_per_sample;          /* 位深 */
    uint32_t bitrate_kbps;             /* 码率 (kbps) */
    uint32_t duration_sec;             /* 总时长 (秒) */
    uint32_t elapsed_sec;              /* 已播放时长 (秒) */
} song_info_t;

extern song_info_t g_song_info;        /* 歌曲信息 (全局实例) */
extern volatile bool g_song_info_valid; /* 信息有效性标志 (先置字段后置此位) */

/* 取一份一致的歌曲信息快照 (与解码任务的成组写入互斥).
 * out=输出; 返回 true=当前信息有效且已完整拷贝; 供 UI 单次一致读取, 避免撕裂 */
bool song_info_snapshot(song_info_t *out);

/* ──────────────────────── 解码器抽象接口 ──────────────────────── */
/* 所有解码器实现同一组函数指针, 音频任务不关心具体格式.
 * self=解码器对象 (各实现自定义结构, 首字段是本接口). */
typedef struct audio_decoder_s {
    bool      (*open)(struct audio_decoder_s *self, const char *path);  /* 打开文件: path=文件路径, 成功返回 true */
    bool      (*decode)(struct audio_decoder_s *self, int16_t *pcm, size_t *bytes);  /* 解码一块: pcm=输出缓冲, bytes 输入容量/输出实际字节数 */
    bool      (*is_eof)(struct audio_decoder_s *self);                  /* 是否到达文件末尾 */
    void      (*close)(struct audio_decoder_s *self);                   /* 关闭并释放 */
    uint32_t  (*get_sample_rate)(struct audio_decoder_s *self);         /* 采样率 */
    uint8_t   (*get_channels)(struct audio_decoder_s *self);            /* 声道数 */
    uint8_t   (*get_bits)(struct audio_decoder_s *self);                /* 位深 */
    uint32_t  (*get_bitrate)(struct audio_decoder_s *self);             /* 码率 */
    uint32_t  (*get_file_size)(struct audio_decoder_s *self);           /* 文件大小 (字节) */
    uint32_t  (*get_position)(struct audio_decoder_s *self);            /* 当前解码位置 (字节); FLAC 为文件读游标, 含预读 */
    bool      (*seek)(struct audio_decoder_s *self, uint32_t byte_offset);  /* 按字节偏移 seek, 成功返回 true */
    const char *(*get_title)(struct audio_decoder_s *self);             /* 歌名 (无则 NULL) */
    const char *(*get_artist)(struct audio_decoder_s *self);            /* 歌手 (无则 NULL) */
    const uint8_t *(*get_cover_data)(struct audio_decoder_s *self);     /* 内嵌封面 JPEG 数据指针 (无则 NULL) */
    size_t         (*get_cover_size)(struct audio_decoder_s *self);     /* 封面数据大小 */
    void           (*take_cover)(struct audio_decoder_s *self);         /* 转移封面数据所有权 (释放后置空, 防止重复释放) */
} audio_decoder_t;

/* 各格式解码器工厂函数 */
audio_decoder_t *decoder_mp3_create(void);
audio_decoder_t *decoder_flac_create(void);
audio_decoder_t *decoder_wav_create(void);

#ifdef __cplusplus
}
#endif

#endif
