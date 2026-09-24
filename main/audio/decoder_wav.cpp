#include <cstdio>
#include <cstring>
#include <cinttypes>
#include <sys/stat.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "audio.h"

#define WAV_TAG "DEC_WAV"

/* 每次最多交给 pipeline 的帧数 (与 MP3/FLAC 的 1152 一致) */
#define WAV_MAX_FRAMES  1152
/* 原始输入缓冲: 1152 帧 × 2ch × 32bit 最大位深 */
#define WAV_CHUNK_RAW   9216
/* LIST/INFO 子块读取上限 */
#define WAV_LIST_MAX    (32 * 1024)

/* WAV 解码器实例 (首字段是公共接口 iface) */
typedef struct {
    audio_decoder_t iface;

    FILE     *file;          /* 文件句柄 */
    uint8_t  *inbuf;         /* PSRAM 原始输入缓冲 */
    bool      eof;           /* 是否已到文件尾 */

    uint32_t sample_rate;    /* 采样率 (来自 fmt 块) */
    uint16_t bits_per_sample;/* 位深 8/16/24/32 */
    uint8_t  channels;       /* 声道数 1/2 */

    uint32_t data_start;     /* PCM 数据起始字节偏移 */
    uint32_t data_size;      /* PCM 数据字节数 (get_file_size) */
    uint32_t data_pos;       /* 已从 data 块消费的源 PCM 字节数 (get_position); 16bit 时等于输出字节数 */
    uint32_t file_size;      /* 整个文件大小 */

    char     title[SONG_TITLE_MAX];    /* 标题 (LIST/INFO INAM) */
    char     artist[SONG_ARTIST_MAX];  /* 作者 (LIST/INFO IART) */
} decoder_wav_t;

/* ── 字节序工具 ── */
/* 读小端 32bit: p=字节指针 */
static uint32_t le32(const uint8_t *p)
{
    return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* 读小端 16bit: p=字节指针 */
static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

/* ── RIFF LIST/INFO 子块: INAM=标题, IART=作者, IPRD=专辑 ── */
/* 提取文本并去掉尾部空白: d=数据, len=长度, out=输出, out_size=缓冲大小 */
static void copy_info_text(const uint8_t *d, uint32_t len, char *out, size_t out_size)
{
    size_t o = 0;
    for (uint32_t i = 0; i < len && o < out_size - 1; i++) {
        if (d[i] == 0) break;   /* 遇 NUL 结束 */
        out[o++] = (char)d[i];
    }
    while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '\t')) o--;   /* 去尾部空白 */
    out[o] = '\0';
}

/* 解析 LIST 块内的 INFO 子块 (INAM/IART/IPRD) */
static void wav_parse_list_info(decoder_wav_t *d, uint32_t size)
{
    long start = ftell(d->file);   /* 记住 LIST 块数据起点 */
    if (size < 4 || size > WAV_LIST_MAX) {   /* 尺寸异常直接跳过 */
        fseek(d->file, start + (long)size + (size & 1), SEEK_SET);   /* +1 对齐 */
        return;
    }
    uint8_t *chunk = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);   /* 整块读入 PSRAM */
    if (!chunk) {
        fseek(d->file, start + (long)size + (size & 1), SEEK_SET);
        return;
    }
    if (fread(chunk, 1, size, d->file) == size) {
        uint32_t p = 0;
        while (p + 8 <= size) {   /* 遍历子块: 每个 = 4字节ID + 4字节长度 + 数据 */
            const uint8_t *h = chunk + p;
            uint32_t fs = le32(h + 4);
            p += 8;
            if (fs > size - p) break;   /* 越界保护 */
            const uint8_t *bd = chunk + p;
            if (!memcmp(h, "INAM", 4) && fs > 0) {   /* 标题 */
                copy_info_text(bd, fs, d->title, sizeof(d->title));
                printf("[WAV] 标题: %s\n", d->title);
            } else if (!memcmp(h, "IART", 4) && fs > 0) {   /* 作者 */
                copy_info_text(bd, fs, d->artist, sizeof(d->artist));
                printf("[WAV] 艺术家: %s\n", d->artist);
            } else if (!memcmp(h, "IPRD", 4) && fs > 0) {   /* 专辑 (仅日志) */
                char buf[96];
                copy_info_text(bd, fs, buf, sizeof(buf));
                printf("[WAV] 专辑: %s\n", buf);
            }
            p += fs;
            if (p & 1) p++;   /* 子块按 2 字节对齐 */
        }
    }
    heap_caps_free(chunk);
    fseek(d->file, start + (long)size + (size & 1), SEEK_SET);   /* 跳回 LIST 数据末尾 */
}

/* 释放上次残留资源 (防止重复 open 泄漏) */
static void wav_free_state(decoder_wav_t *d)
{
    if (d->file) { fclose(d->file); d->file = NULL; }
    if (d->inbuf) { heap_caps_free(d->inbuf); d->inbuf = NULL; }
    d->eof = false;
    d->data_pos = 0;
    d->data_start = 0;
    d->data_size = 0;
}

/* 打开 WAV 文件: 校验 RIFF/WAVE 头, 解析 fmt/data/LIST 块 */
static bool wav_open(audio_decoder_t *iface, const char *path)
{
    decoder_wav_t *d = (decoder_wav_t *)iface;
    wav_free_state(d);

    FILE *f = fopen(path, "rb");
    if (!f) return false;
    d->file = f;

    {   /* 记录文件总大小 */
        struct stat st;
        if (stat(path, &st) == 0) d->file_size = (uint32_t)st.st_size;
    }

    /* RIFF 头: "RIFF" + size + "WAVE" */
    uint8_t h[12];
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
        wav_free_state(d);
        return false;
    }

    bool have_fmt = false, have_data = false;   /* 是否解析到 fmt 和 data 块 */

    /* 遍历各块直到 data 块 (最多 64 块防呆) */
    for (int guard = 0; guard < 64; guard++) {
        uint8_t ch[8];
        if (fread(ch, 1, 8, f) != 8) break;   /* 块头 = 4字节ID + 4字节长度 */
        uint32_t size = le32(ch + 4);
        uint32_t cur = (uint32_t)ftell(f);
        uint32_t remain = (d->file_size > cur) ? d->file_size - cur : 0;
        if (size > remain) size = remain;   /* 防块长度越界文件 */

        if (!memcmp(ch, "fmt ", 4)) {   /* 格式块 */
            if (size < 16) { wav_free_state(d); return false; }
            uint8_t fh[40] = {0};
            size_t to_read = size < 40 ? size : 40;   /* 只读前 40 字节 (含扩展头) */
            if (fread(fh, 1, to_read, f) != to_read) { wav_free_state(d); return false; }
            uint16_t afmt = le16(fh);               /* 编码格式: 1=PCM */
            d->channels = (uint8_t)le16(fh + 2);
            d->sample_rate = le32(fh + 4);
            d->bits_per_sample = le16(fh + 14);
            if (afmt == 0xFFFE && to_read >= 40) afmt = le16(fh + 24);   /* WAVE_FORMAT_EXTENSIBLE 取真实格式 */
            if (afmt != 1 || d->channels < 1 || d->channels > 2 ||   /* 只支持 PCM 单/双声道 */
                d->sample_rate == 0 ||
                (d->bits_per_sample != 8 && d->bits_per_sample != 16 &&
                 d->bits_per_sample != 24 && d->bits_per_sample != 32)) {
                wav_free_state(d);
                return false;
            }
            if (size > to_read) fseek(f, (long)(size - to_read), SEEK_CUR);   /* 跳过剩余 */
            have_fmt = true;
            continue;
        }

        if (!memcmp(ch, "data", 4)) {   /* 数据块: 记录偏移和大小 */
            d->data_start = (uint32_t)ftell(f);
            d->data_size = size;
            have_data = true;
            break;
        }

        if (!memcmp(ch, "LIST", 4)) {   /* 元数据块 */
            wav_parse_list_info(d, size);
            continue;
        }

        fseek(f, (long)size + (size & 1), SEEK_CUR);   /* 跳过未知块 */
    }

    if (!have_fmt || !have_data) { wav_free_state(d); return false; }
    if (d->data_size == 0 || d->data_size == 0xFFFFFFFFu) {   /* data 块长度未知则按文件尾 */
        d->data_size = d->file_size - d->data_start;
    }
    if (d->data_start + d->data_size > d->file_size) {   /* 越界保护 */
        d->data_size = d->file_size - d->data_start;
    }

    d->inbuf = (uint8_t *)heap_caps_malloc(WAV_CHUNK_RAW, MALLOC_CAP_SPIRAM);   /* 输入缓冲 */
    if (!d->inbuf) { wav_free_state(d); return false; }

    uint32_t frame_bytes = d->channels * (d->bits_per_sample / 8);   /* 每帧字节数 */
    printf("[WAV] %" PRIu32 " Hz, %u ch, %u bit, data=%" PRIu32 " bytes (%.1f s)\n",
           d->sample_rate, d->channels, d->bits_per_sample, d->data_size,
           d->sample_rate && frame_bytes ? (double)d->data_size / ((double)d->sample_rate * frame_bytes) : 0.0);
    return true;
}

/* 解码一帧: 读原始字节 → 转换位深到 16bit 输出.
 * pcm=输出缓冲, bytes 输入容量/输出实际字节数 */
static bool wav_decode_frame(audio_decoder_t *iface, int16_t *pcm, size_t *bytes)
{
    decoder_wav_t *d = (decoder_wav_t *)iface;
    if (!d->file || !d->inbuf) return false;

    uint32_t frame_bytes = d->channels * (d->bits_per_sample / 8);
    if (frame_bytes == 0) { d->eof = true; return false; }

    uint32_t remain = d->data_size - d->data_pos;   /* 剩余数据 */
    if (remain == 0) { d->eof = true; return false; }

    uint32_t raw = WAV_MAX_FRAMES * frame_bytes;   /* 本次最多读这么多字节 */
    if (raw > remain) raw = remain;                /* 最后一块只读剩余 */
    if (raw == 0) { d->eof = true; return false; }

    size_t n = fread(d->inbuf, 1, raw, d->file);
    if (n == 0) { d->eof = true; return false; }
    d->data_pos += (uint32_t)n;
    uint32_t samples = (uint32_t)n / frame_bytes * d->channels;   /* 完整样本数 */
    if (samples == 0) { d->eof = true; return false; }

    const uint8_t *p = d->inbuf;
    switch (d->bits_per_sample) {   /* 各种位深 → 16bit */
    case 8:   /* 无符号 8bit: 减 128 后左移 8 位 */
        for (uint32_t i = 0; i < samples; i++) {
            pcm[i] = (int16_t)(((int16_t)p[i] - 128) << 8);
        }
        break;
    case 16:   /* 原样拷贝 */
        memcpy(pcm, d->inbuf, n);
        break;
    case 24:   /* 3 字节小端 → 取高 16 位 */
        for (uint32_t i = 0; i < samples; i++) {
            int32_t v = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16));
            pcm[i] = (int16_t)(v >> 8);
            p += 3;
        }
        break;
    case 32:   /* 4 字节小端 → 取高 16 位 */
        for (uint32_t i = 0; i < samples; i++) {
            int32_t v = (int32_t)le32(p);
            pcm[i] = (int16_t)(v >> 16);
            p += 4;
        }
        break;
    default:
        d->eof = true;
        return false;
    }

    *bytes = (size_t)samples * sizeof(int16_t);
    return true;
}

/* 是否已到 data 区末尾 */
static bool wav_is_eof(audio_decoder_t *iface)
{
    return ((decoder_wav_t *)iface)->eof;
}

/* 关闭并释放文件/缓冲 */
static void wav_close(audio_decoder_t *iface)
{
    wav_free_state((decoder_wav_t *)iface);
}

/* 采样率 (Hz) */
static uint32_t wav_get_sample_rate(audio_decoder_t *iface)
{
    return ((decoder_wav_t *)iface)->sample_rate;
}

/* 声道数 */
static uint8_t wav_get_channels(audio_decoder_t *iface)
{
    return ((decoder_wav_t *)iface)->channels;
}

/* 位深 (8/16/24/32) */
static uint8_t wav_get_bits(audio_decoder_t *iface)
{
    return ((decoder_wav_t *)iface)->bits_per_sample;
}

/* 精确码率(kbps): 采样率×声道×位深/1000 */
static uint32_t wav_get_bitrate(audio_decoder_t *iface)
{
    decoder_wav_t *d = (decoder_wav_t *)iface;
    return (uint32_t)((uint64_t)d->sample_rate * d->channels * d->bits_per_sample / 1000);
}

/* 只报告 PCM 数据字节数 → duration_sec/elapsed_sec 精确 */
static uint32_t wav_get_file_size(audio_decoder_t *iface)
{
    return ((decoder_wav_t *)iface)->data_size;
}

/* 已消费的源 PCM 字节数 (16bit 时即输出字节数) */
static uint32_t wav_get_position(audio_decoder_t *iface)
{
    return ((decoder_wav_t *)iface)->data_pos;
}

/* 按字节偏移 seek: byte_offset=相对 data 区起始的偏移 */
static bool wav_seek(audio_decoder_t *iface, uint32_t byte_offset)
{
    decoder_wav_t *d = (decoder_wav_t *)iface;
    if (!d->file) return false;
    if (byte_offset > d->data_size) byte_offset = d->data_size;   /* 钳制 */
    if (fseek(d->file, (long)(d->data_start + byte_offset), SEEK_SET) != 0) return false;
    d->data_pos = byte_offset;
    d->eof = false;
    return true;
}

/* 标题 (LIST/INFO INAM) */
static const char *wav_get_title(audio_decoder_t *iface)
{
    return ((decoder_wav_t *)iface)->title;
}

/* 作者 (LIST/INFO IART) */
static const char *wav_get_artist(audio_decoder_t *iface)
{
    return ((decoder_wav_t *)iface)->artist;
}

/* 工厂: 创建 WAV 解码器, 填充接口函数指针 (WAV 无内嵌封面, 相关接口置空) */
audio_decoder_t *decoder_wav_create(void)
{
    decoder_wav_t *d = (decoder_wav_t *)calloc(1, sizeof(decoder_wav_t));
    if (!d) return NULL;

    d->iface.open            = wav_open;
    d->iface.decode          = wav_decode_frame;
    d->iface.is_eof          = wav_is_eof;
    d->iface.close           = wav_close;
    d->iface.get_sample_rate = wav_get_sample_rate;
    d->iface.get_channels    = wav_get_channels;
    d->iface.get_bits        = wav_get_bits;
    d->iface.get_bitrate     = wav_get_bitrate;
    d->iface.get_file_size   = wav_get_file_size;
    d->iface.get_position    = wav_get_position;
    d->iface.seek            = wav_seek;
    d->iface.get_title       = wav_get_title;
    d->iface.get_artist      = wav_get_artist;
    d->iface.get_cover_data  = NULL;
    d->iface.get_cover_size  = NULL;
    d->iface.take_cover      = NULL;

    return &d->iface;
}
