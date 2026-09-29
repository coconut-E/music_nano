#include <cstdio>
#include <cstring>
#include <cinttypes>
#include <cstdlib>
#include <strings.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "audio_task.h"
#include "audio.h"
#include "pcm_pipeline.h"
#include "atomic_utils.h"
#include "cover.h"
#include "sys_monitor.h"
#include "settings.h"

#define AUDIO_TAG "AUDIO"

/* MPEG1 stereo: 1152 samples × 2ch = 2304 int16_t (一帧最大 PCM 样本数) */
#define MP3_PCM_BUF_SAMPLES  (1152 * 2)

/* 44.1kHz stereo 16bit 输出缓冲, 容纳最坏重采样结果.
 * 最坏: 1152 帧源 @8kHz → 1152*44100/8000 ≈ 6350 输出帧; 取 8192 帧 (32KB) 留余量,
 * 配合 pcm_pipeline 的最低源率 8kHz 限制, 保证单块输出不被截断 */
#define PCM_OUT_BUF_SAMPLES  (8192 * 2)

extern "C" {
volatile bool g_pcm_active = false;         /* 是否正在推流 (供 UI/电源判断播放中) */
volatile bool g_audio_decoder_open = false; /* 解码器是否已打开 (可能占用 SD 文件) */
song_info_t g_song_info = {};               /* 当前歌曲信息 (解码任务写, UI 读) */
volatile bool g_song_info_valid = false;    /* 信息有效标志 */
}

/* 音频任务内部状态机 */
typedef enum {
    STATE_IDLE = 0,     /* 空闲: 无解码器/未播放 */
    STATE_PLAYING,      /* 播放中: 解码→转换→推流 */
    STATE_PAUSED,       /* 暂停: 保留解码器但停止推流 */
} audio_state_t;

static QueueHandle_t        s_cmd_queue   = NULL;   /* 命令队列 (UI→音频) */
static QueueHandle_t        s_rsp_queue   = NULL;   /* 应答队列 (音频→UI) */
static StreamBufferHandle_t s_pcm_stream  = NULL;   /* 蓝牙 PCM 流缓冲 */
static audio_state_t        s_state       = STATE_IDLE;   /* 当前状态 */
static TaskHandle_t         s_task        = NULL;   /* 音频任务句柄 (模式切换挂起用) */
static volatile bool        s_task_ready  = false;  /* 音频任务已进入主循环 (可安全挂起) */

static audio_decoder_t     *s_decoder     = NULL;   /* 当前解码器对象 */
static char                 s_current_path[256];    /* 当前播放文件路径 */

/* ── SD 预读流: 音频任务持文件句柄 + PSRAM 预读缓冲 (解码器不再直接碰 FILE*) ──
 * 单次 SD 读失败只是"这一轮没填上", 解码器仍从缓冲继续消费, 不会立刻误判播放结束;
 * 仅当文件真正到尾, 或缓冲排空后持续读失败超过 ASTREAM_RETRY_MS 才返回 0. */
#define ASTREAM_CAP        (20 * 1024)   /* 预读缓冲大小 (PSRAM) */
#define ASTREAM_PUMP_FREE  (5 * 1024)    /* 空余 >= 此值就补读, 保持预读领先 */
#define ASTREAM_READ_CHUNK (16 * 1024)   /* 单次 SD 读取上限 */
#define ASTREAM_RETRY_MS   500          /* 缓冲排空后持续重试上限 (ms) */

typedef struct {
    audio_stream_t iface;
    FILE          *f;
    uint8_t       *buf;       /* PSRAM 线性缓冲 (任务启动时分配一次, 跨歌复用) */
    size_t         cap;
    size_t         start;     /* 有效数据起始下标 */
    size_t         fill;      /* 有效字节数 */
    uint32_t       logical;   /* 逻辑读位置 (已交付给解码器的下一字节的文件偏移) */
    uint32_t       fsize;     /* 文件总大小 */
    bool           feof_file; /* 底层文件已到尾 */
} astream_t;

static astream_t s_astream;

/* 从 SD 读一块进缓冲; 有进展返回 true */
static bool astream_pump(void)
{
    astream_t *s = &s_astream;
    if (!s->f || s->feof_file) return false;

    /* 尾部空间不够就把有效数据压缩到头部 */
    if (s->cap - s->start - s->fill < 512 && s->start > 0) {
        if (s->fill) memmove(s->buf, s->buf + s->start, s->fill);
        s->start = 0;
    }
    size_t free_sp = s->cap - s->start - s->fill;
    if (free_sp < 512) return false;
    size_t want = free_sp < ASTREAM_READ_CHUNK ? free_sp : ASTREAM_READ_CHUNK;

    size_t rd = 0;
    sd_fs_lock();
    if (s->f) rd = fread(s->buf + s->start + s->fill, 1, want, s->f);
    sd_fs_unlock();

    if (rd > 0) { s->fill += rd; return true; }
    if (s->f && feof(s->f)) s->feof_file = true;   /* 真到文件尾 */
    return false;
}

/* 顺序读: 尽量读满 len; 仅真到尾/持续失败才短读或返回 0 */
static size_t astream_read(struct audio_stream_s *self, void *dst, size_t len)
{
    astream_t *s = (astream_t *)self;
    uint8_t *out = (uint8_t *)dst;
    size_t done = 0;
    int64_t t0 = esp_timer_get_time();

    while (done < len) {
        if (s->fill == 0) {
            if (astream_pump()) continue;
            if (s->feof_file) break;                                        /* 真到尾 */
            if (esp_timer_get_time() - t0 > (int64_t)ASTREAM_RETRY_MS * 1000)
                break;                                                      /* 持续失败, 放弃 */
            vTaskDelay(pdMS_TO_TICKS(5));                                   /* 稍后重试 */
            continue;
        }
        size_t n = len - done;
        if (n > s->fill) n = s->fill;
        memcpy(out + done, s->buf + s->start, n);
        s->start   += n;
        s->fill    -= n;
        s->logical += (uint32_t)n;
        done       += n;
    }
    if (s->fill == 0) s->start = 0;

    /* 预读: 空余 >=5KB 就补上, 提前为下一块备数据 (失败静默跳过, 下轮再试) */
    while (s->cap - s->start - s->fill >= ASTREAM_PUMP_FREE) {
        if (!astream_pump()) break;
    }
    return done;
}

/* 定位: 清空缓冲 + 底层 fseek */
static bool astream_seek(struct audio_stream_s *self, long off, int whence)
{
    astream_t *s = (astream_t *)self;
    if (!s->f) return false;

    long target;
    if (whence == SEEK_SET)      target = off;
    else if (whence == SEEK_CUR) target = (long)s->logical + off;
    else                         target = (long)s->fsize + off;
    if (target < 0) target = 0;
    if ((uint32_t)target > s->fsize) target = (long)s->fsize;

    bool ok;
    sd_fs_lock();
    ok = (s->f && fseek(s->f, target, SEEK_SET) == 0);
    sd_fs_unlock();
    if (!ok) return false;

    s->start = 0;
    s->fill  = 0;
    s->logical = (uint32_t)target;
    s->feof_file = false;
    return true;
}

static long astream_tell(struct audio_stream_s *self)
{
    return (long)((astream_t *)self)->logical;
}

static uint32_t astream_size(struct audio_stream_s *self)
{
    return ((astream_t *)self)->fsize;
}

/* 任务启动时分配预读缓冲并绑定接口 (只做一次) */
static bool astream_init(void)
{
    memset(&s_astream, 0, sizeof(s_astream));
    s_astream.iface.read = astream_read;
    s_astream.iface.seek = astream_seek;
    s_astream.iface.tell = astream_tell;
    s_astream.iface.size = astream_size;
    s_astream.buf = (uint8_t *)heap_caps_malloc(ASTREAM_CAP, MALLOC_CAP_SPIRAM);
    if (!s_astream.buf) {
        s_astream.buf = (uint8_t *)heap_caps_malloc(ASTREAM_CAP, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    }
    if (!s_astream.buf) return false;
    s_astream.cap = ASTREAM_CAP;
    return true;
}

/* 打开底层文件并复位游标 (缓冲由 astream_init 持有, 不清空) */
static bool astream_open(const char *path)
{
    astream_t *s = &s_astream;
    s->start = 0; s->fill = 0; s->logical = 0; s->fsize = 0;
    s->feof_file = false; s->f = NULL;

    sd_fs_lock();
    FILE *f = fopen(path, "rb");
    if (f) {
        struct stat st;
        if (stat(path, &st) == 0) s->fsize = (uint32_t)st.st_size;
        s->f = f;
    }
    sd_fs_unlock();
    if (!f) return false;
    atomic_store_bool(&g_audio_decoder_open, true);   /* 已占用 SD 文件, 阻止卸载 */
    return true;
}

/* 关闭底层文件 (缓冲区保留复用) */
static void astream_close(void)
{
    astream_t *s = &s_astream;
    sd_fs_lock();
    if (s->f) { fclose(s->f); s->f = NULL; }
    sd_fs_unlock();
    s->start = 0; s->fill = 0; s->logical = 0; s->fsize = 0; s->feof_file = false;
    atomic_store_bool(&g_audio_decoder_open, false);
}

/* ── PCM 输出 ── */
static bool     s_pending_pcm = false;   /* 是否有一块已转换待发送的 PCM */
static int16_t *s_pcm_buf = NULL;        /* 解码原始 PCM 缓冲 */
static size_t   s_pcm_bytes   = 0;       /* 当前块解码字节数 */
static size_t   s_pcm_offset  = 0;       /* 已写入蓝牙流的偏移 */

/* ── 解码 -> 蓝牙 中间转换层 ── */
static pcm_pipeline_t *s_pipeline  = NULL;   /* 转换层对象 */
static uint32_t        s_pipe_rate = 0;      /* 转换层已配置的采样率 */
static uint8_t         s_pipe_ch   = 0;      /* 转换层已配置的声道数 */
static int16_t        *s_out_buf = NULL;     /* 转换输出缓冲 */
static size_t          s_out_bytes = 0;      /* 当前块转换输出字节数 */
static bool            s_info_done = false;  /* 歌曲信息是否已解析完成 */
static SemaphoreHandle_t s_info_mux = NULL;  /* 保护 g_song_info 的成组写入/快照读取 (防撕裂) */

static uint64_t s_total_decoded = 0;   /* 累计解码字节数 (统计用) */
static uint64_t s_total_sent    = 0;   /* 累计发送字节数 (统计用) */
static int64_t  s_play_start_us = 0;   /* 本次播放开始时刻 (统计耗时用) */

/* ── 辅助函数 ── */
/* 关闭并释放当前解码器 */
static void close_decoder(void)
{
    if (s_decoder) {
        audio_decoder_t *d = s_decoder;
        s_decoder = NULL;
        d->close(d);        /* 释放解码器自身资源 (缓冲/封面; 不再 fclose) */
        astream_close();    /* 关闭底层文件 + 清占用标志 (内部持 sd_fs_lock) */
        free(d);            /* 释放对象本体 */
    }
}

/* 按扩展名选择解码器并打开文件: path=文件路径, 成功返回 true.
 * 同时把内嵌封面交给封面解码任务 (所有权转移). */
static bool open_decoder(const char *path)
{
    if (!atomic_load_bool(&g_sd_ready)) {   /* SD 未就绪: 拒绝打开, 避免占用已下线 FS */
        printf("[音频] SD 未就绪, 无法打开 %s\n", path);
        return false;
    }

    /* 根据扩展名选解码器工厂 */
    const char *dot = strrchr(path, '.');
    if (dot && strcasecmp(dot, ".flac") == 0) {
        s_decoder = decoder_flac_create();
    } else if (dot && strcasecmp(dot, ".wav") == 0) {
        s_decoder = decoder_wav_create();
    } else {   /* 默认 MP3 (含无扩展名/其他) */
        s_decoder = decoder_mp3_create();
    }
    if (!s_decoder) {
        printf("[音频] 创建解码器失败\n");
        return false;
    }
    /* 打开底层文件流 (内部持 sd_fs_lock); 复核 SD 仍就绪后交给解码器解析头部 */
    bool opened = atomic_load_bool(&g_sd_ready) && astream_open(path)
                  && s_decoder->open(s_decoder, path, &s_astream.iface);
    if (!opened) {
        printf("[音频] 无法打开 %s\n", path);
        astream_close();
        free(s_decoder);
        s_decoder = NULL;
        return false;
    }
    strncpy(s_current_path, path, sizeof(s_current_path) - 1);   /* 记住当前路径 */
    s_current_path[sizeof(s_current_path) - 1] = '\0';

    /* 有封面则交给解码任务 (所有权移交, 解码完成后释放) */
    const uint8_t *cd = s_decoder->get_cover_data ? s_decoder->get_cover_data(s_decoder) : NULL;
    size_t cs = s_decoder->get_cover_size ? s_decoder->get_cover_size(s_decoder) : 0;
    if (cd && cs > 0) {
        /* 仅在封面模块真正接管所有权后才 take_cover; 未接管则由解码器 close 释放 */
        if (cover_submit_job(cd, cs)) {
            s_decoder->take_cover(s_decoder);  /* 转移所有权, 防重复释放 */
        }
    } else {
        cover_notify_no_cover();           /* 无封面, 通知 UI 回退默认图标 */
    }

    printf("[音频] 解码器已加载: %s\n", path);
    return true;
}

/* 由文件扩展名填格式名 */
static void set_format_from_path(const char *path)
{
    const char *fmt = "MP3";
    const char *dot = strrchr(path, '.');
    if (dot) {
        if      (strcasecmp(dot, ".flac") == 0) fmt = "FLAC";
        else if (strcasecmp(dot, ".wav")  == 0) fmt = "WAV";
        else if (strcasecmp(dot, ".aac")  == 0) fmt = "AAC";
        else if (strcasecmp(dot, ".mp3")  == 0) fmt = "MP3";
    }
    strncpy(g_song_info.format, fmt, SONG_FORMAT_MAX - 1);
    g_song_info.format[SONG_FORMAT_MAX - 1] = '\0';
}

/* 更新码率/总时长/已播时长 (按平均码率估算). 要求调用方已持有 s_info_mux */
static void update_duration_elapsed_locked(void)
{
    uint32_t kbps = s_decoder->get_bitrate(s_decoder);
    uint32_t bps  = kbps * 125;   /* kbps → 字节每秒 */
    if (bps == 0) bps = 128 * 125;   /* 未知码率按 128kbps 兜底 */

    g_song_info.bitrate_kbps = kbps ? kbps : 128;
    g_song_info.duration_sec = s_decoder->get_file_size(s_decoder) / bps;   /* 文件大小/码率=时长 */
    g_song_info.elapsed_sec  = s_decoder->get_position(s_decoder) / bps;    /* 已解码字节/码率 */
}

/* 独立调用版本: 自行加锁 (decode 循环里周期性更新进度) */
static void update_duration_elapsed(void)
{
    if (s_info_mux) xSemaphoreTake(s_info_mux, portMAX_DELAY);
    update_duration_elapsed_locked();
    if (s_info_mux) xSemaphoreGive(s_info_mux);
}

/* 填充完整歌曲信息 (标题/歌手/格式/参数), 最后原子置有效标志.
 * 全程持锁, 保证 UI 快照读到的是同一首歌的一致信息 */
static void fill_song_info(void)
{
    if (s_info_mux) xSemaphoreTake(s_info_mux, portMAX_DELAY);
    const char *title  = s_decoder->get_title(s_decoder);
    const char *artist = s_decoder->get_artist(s_decoder);

    if (title && title[0]) {
        strncpy(g_song_info.title, title, SONG_TITLE_MAX - 1);
        g_song_info.title[SONG_TITLE_MAX - 1] = '\0';
    } else {   /* 无标签 → 用文件名去扩展名当标题 */
        const char *base = strrchr(s_current_path, '/');
        base = base ? base + 1 : s_current_path;
        strncpy(g_song_info.title, base, SONG_TITLE_MAX - 1);
        g_song_info.title[SONG_TITLE_MAX - 1] = '\0';
        char *dot = strrchr(g_song_info.title, '.');
        if (dot) *dot = '\0';
    }

    if (artist && artist[0]) {
        strncpy(g_song_info.artist, artist, SONG_ARTIST_MAX - 1);
        g_song_info.artist[SONG_ARTIST_MAX - 1] = '\0';
    } else {
        g_song_info.artist[0] = '\0';   /* 无歌手留空 */
    }

    set_format_from_path(s_current_path);

    g_song_info.sample_rate  = s_decoder->get_sample_rate(s_decoder);
    g_song_info.channels     = s_decoder->get_channels(s_decoder);
    g_song_info.bits_per_sample = (s_decoder->get_bits ? s_decoder->get_bits(s_decoder) : 16);
    update_duration_elapsed_locked();   /* 已持锁, 用 _locked 版本避免重复加锁 */

    atomic_store_bool(&g_song_info_valid, true);   /* 最后才置有效位 (先写字段后发信号) */
    printf("[音频] 信息: %s | %s | %s | %" PRIu32 "Hz/%uch/%ubit/%" PRIu32 "kbps | %" PRIu32 "s\n",
           g_song_info.title, g_song_info.artist, g_song_info.format,
           g_song_info.sample_rate, g_song_info.channels,
           g_song_info.bits_per_sample,
           g_song_info.bitrate_kbps, g_song_info.duration_sec);
    if (s_info_mux) xSemaphoreGive(s_info_mux);
}

/* 取一份一致的歌曲信息快照: 与 fill_song_info 的成组写入互斥 */
extern "C" bool song_info_snapshot(song_info_t *out)
{
    if (!out) return false;
    if (s_info_mux) xSemaphoreTake(s_info_mux, portMAX_DELAY);
    bool valid = atomic_load_bool(&g_song_info_valid);
    if (valid) *out = g_song_info;   /* 整体拷贝 (含字符串数组), 保证一致 */
    if (s_info_mux) xSemaphoreGive(s_info_mux);
    return valid;
}

/* 启动播放: 需要则切换解码器; 失败则发 FILE_NOT_FOUND 并停在 IDLE.
 * path=要播放的文件路径 */
static void start_play(const char *path)
{
    if (s_decoder && strcmp(s_current_path, path) != 0) {
        close_decoder();   /* 换歌: 释放旧解码器 */
    }
    if (!s_decoder) {
        if (!open_decoder(path)) {
            audio_rsp_t rsp;
            rsp.type = AUDIO_RSP_FILE_NOT_FOUND;   /* 通知 UI 文件不存在 */
            xQueueSend(s_rsp_queue, &rsp, 0);
            s_state = STATE_IDLE;
            return;
        }
    }

    /* 重置所有播放状态 */
    atomic_store_bool(&g_song_info_valid, false);
    s_pipe_rate = 0;
    s_pipe_ch   = 0;
    s_info_done = false;
    s_pending_pcm = false;
    s_total_decoded = 0;
    s_total_sent = 0;
    s_play_start_us = esp_timer_get_time();
    s_state = STATE_PLAYING;
    printf("[音频] 进入STATE_PLAYING | ts=%lld us\n", s_play_start_us);
}

/* ──────────────────────────────────────────────
 *  音频任务
 * ────────────────────────────────────────────── */
static void audio_task(void *arg)
{
    audio_cmd_t cmd;

    s_task_ready = true;   /* 已进入主循环: 允许模式切换时安全挂起 */

    while (1) {
        atomic_store_bool(&g_pcm_active, (s_state == STATE_PLAYING));   /* 更新全局播放标志 */
        atomic_store_bool(&g_audio_decoder_open, s_decoder != NULL);    /* 解码器是否占用 SD 文件 */

        /* SD 已拔出/未就绪: 立即停止占用 SD 并回空闲 (不依赖 UI 的 STOP 时序) */
        if (s_decoder && !atomic_load_bool(&g_sd_ready)) {
            xStreamBufferReset(s_pcm_stream);
            s_pending_pcm = false;
            s_pcm_offset = 0;
            close_decoder();
            atomic_store_bool(&g_audio_decoder_open, false);
            atomic_store_bool(&g_song_info_valid, false);
            s_state = STATE_IDLE;
            printf("[音频] SD 移除, 停止播放并释放文件\n");
            continue;
        }

        switch (s_state) {

        /* ─── 空闲态: 等命令 ─── */
        case STATE_IDLE:
            if (xQueueReceive(s_cmd_queue, &cmd, 0) != pdTRUE) { vTaskDelay(pdMS_TO_TICKS(10)); break; }

            switch (cmd.type) {
            case AUDIO_CMD_PLAY:
                start_play(cmd.path);   /* 开始播放 */
                break;
            case AUDIO_CMD_STOP:
                close_decoder();
                atomic_store_bool(&g_song_info_valid, false);
                break;
            case AUDIO_CMD_PAUSE:
            case AUDIO_CMD_BT_CONNECTED:
            case AUDIO_CMD_BT_DISCONNECTED:
            default:
                break;   /* 空闲态忽略这些命令 */
            }
            break;

        /* ─── 播放中 ─── */
        case STATE_PLAYING: {
            /* (1) 先查队列命令(非阻塞), 一次收完 */
            bool changed = false;
            while (xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE) {
                switch (cmd.type) {
                case AUDIO_CMD_PLAY:
                    if (strcmp(s_current_path, cmd.path) != 0) {   /* 换歌 */
                        xStreamBufferReset(s_pcm_stream);   /* 清空蓝牙流, 避免旧歌残音 */
                        s_pending_pcm = false;
                        start_play(cmd.path);
                    }
                    break;
                case AUDIO_CMD_STOP:
                    xStreamBufferReset(s_pcm_stream); s_pending_pcm = false;
                    close_decoder();
                    atomic_store_bool(&g_song_info_valid, false);
                    s_state = STATE_IDLE;
                    break;
                case AUDIO_CMD_PAUSE:
                    xStreamBufferReset(s_pcm_stream); s_pending_pcm = false;
                    /* 暂停只停 PCM 输出, 音乐头信息仍要解析 */
                    if (s_decoder && !s_info_done) {
                        size_t bytes = 0;
                        bool ok = s_decoder->decode(s_decoder, s_pcm_buf, &bytes) && bytes > 0;
                        if (ok) {
                            fill_song_info();   /* 利用暂停前补解析歌曲信息 */
                            s_info_done = true;
                        }
                    }
                    s_state = STATE_PAUSED;
                    break;
                case AUDIO_CMD_SEEK:
                    if (s_decoder && s_decoder->seek) {
                        uint32_t fsz = s_decoder->get_file_size(s_decoder);
                        uint32_t target = (uint64_t)fsz * cmd.param / 1000;   /* param=千分比 → 字节偏移 */
                        s_decoder->seek(s_decoder, target);
                        xStreamBufferReset(s_pcm_stream);
                        s_pending_pcm = false;
                        s_pcm_offset = 0;
                        if (s_info_mux) xSemaphoreTake(s_info_mux, portMAX_DELAY);
                        g_song_info.elapsed_sec =   /* 同步进度显示 (与快照读取互斥) */
                            (uint64_t)g_song_info.duration_sec * cmd.param / 1000;
                        if (s_info_mux) xSemaphoreGive(s_info_mux);
                    }
                    break;
                case AUDIO_CMD_BT_CONNECTED:
                    /* 连接建立时清空旧流, 让 A2DP 从帧边界起全新数据开编 (修复沙沙声) */
                    xStreamBufferReset(s_pcm_stream);
                    s_pending_pcm = false;
                    s_pcm_offset = 0;
                    printf("[音频] BT 已连接, 重置 PCM 流\n");
                    break;
                case AUDIO_CMD_BT_DISCONNECTED:
                    xStreamBufferReset(s_pcm_stream); s_pending_pcm = false;
                    s_state = STATE_PAUSED;   /* 蓝牙断开 → 挂起, 等待重连 */
                    break;
                default:
                    break;
                }
                if (s_state != STATE_PLAYING) { changed = true; break; }   /* 状态变了退出命令循环 */
            }
            if (changed) break;

            /* (2) 无待发 PCM → 解码一帧 */
            if (!s_pending_pcm) {
                if (!s_decoder) {
                    s_state = STATE_IDLE;
                    break;
                }
                s_pcm_bytes = MP3_PCM_BUF_SAMPLES * sizeof(int16_t);   /* 传入缓冲容量 (解码器据此限幅) */
                bool dec_ok = s_decoder->decode(s_decoder, s_pcm_buf, &s_pcm_bytes);
                if (!dec_ok) {
                    if (s_decoder->is_eof(s_decoder)) {   /* 解码失败且到 EOF = 播放完毕 */
                        int64_t elapsed = esp_timer_get_time() - s_play_start_us;
                        printf("[音频] 播放完毕 | 解码=%" PRIu64 " 发送=%" PRIu64 " 耗时=%lld us (%.2f s)\n",
                               s_total_decoded, s_total_sent, elapsed, elapsed / 1000000.0);
                        close_decoder();
                        atomic_store_bool(&g_song_info_valid, false);
                        s_state = STATE_IDLE;

                        audio_rsp_t rsp;
                        rsp.type = AUDIO_RSP_SONG_FINISHED;   /* 通知 UI 切下一首 */
                        xQueueSend(s_rsp_queue, &rsp, 0);
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(10));   /* 暂时无数据, 稍后重试 */
                    break;
                }
                s_total_decoded += s_pcm_bytes;

                /* 用解码器输出的采样率/声道配置转换层 (固定转 44.1k/16bit/stereo) */
                uint32_t rate = s_decoder->get_sample_rate(s_decoder);
                uint8_t  ch   = s_decoder->get_channels(s_decoder);
                if (!s_pipeline) {
                    s_pipeline = pcm_pipeline_create();
                    if (!s_pipeline) {
                        printf("[音频] 转换层创建失败\n");
                        close_decoder();
                        atomic_store_bool(&g_song_info_valid, false);
                        s_state = STATE_IDLE;
                        break;
                    }
                }
                if (rate != s_pipe_rate || ch != s_pipe_ch) {   /* 源格式变了才重配 */
                    if (!pcm_pipeline_open(s_pipeline, rate, 16, ch)) {
                        printf("[音频] 转换层初始化失败 rate=%" PRIu32 " ch=%u\n", rate, ch);
                        close_decoder();
                        atomic_store_bool(&g_song_info_valid, false);
                        s_state = STATE_IDLE;
                        break;
                    }
                    s_pipe_rate = rate;
                    s_pipe_ch   = ch;
                    printf("[音频] 转换层: %" PRIu32 "Hz/%uch -> 44100Hz/2ch\n", rate, ch);
                }

                if (!s_info_done) {   /* 首次填充完整信息, 之后只更新时长/进度 */
                    fill_song_info();
                    s_info_done = true;
                } else {
                    update_duration_elapsed();
                }

                /* 转换到 44.1k stereo 输出缓冲 (顺带施加软件音量增益) */
                size_t frames = s_pcm_bytes / (ch * 2);   /* 帧数 = 字节/(声道数×2字节/样本) */
                s_out_bytes = pcm_pipeline_process(s_pipeline, s_pcm_buf, frames,
                                                   (uint8_t*)s_out_buf, PCM_OUT_BUF_SAMPLES * sizeof(int16_t),
                                                   (uint8_t)volume_get_gain());

                s_pending_pcm = true;
                s_pcm_offset = 0;
            }

            /* (3) 非阻塞填入 PCM, 填多少算多少 (缓冲满则只填一部分) */
            {
                size_t written = xStreamBufferSend(s_pcm_stream,
                    (const uint8_t*)s_out_buf + s_pcm_offset,
                    s_out_bytes - s_pcm_offset, 0);
                if (written > 0) {
                    s_pcm_offset += written;
                }
            }

            /* (4) 未填完 → 释放 CPU 1ms 回主循环(缓冲满, 下轮只查命令+继续填, 不解码) */
            if (s_pcm_offset < s_out_bytes) {
                vTaskDelay(pdMS_TO_TICKS(1));
                break;
            }

            /* (5) 已填完 → 清 pending, 下轮进入解码 */
            s_total_sent += s_out_bytes;
            s_pending_pcm = false;
            break;
        }

        /* ─── 暂停中 ─── */
        case STATE_PAUSED:
            if (xQueueReceive(s_cmd_queue, &cmd, 0) != pdTRUE) { vTaskDelay(pdMS_TO_TICKS(10)); break; }

            switch (cmd.type) {
            case AUDIO_CMD_PLAY:
                if (s_decoder && strcmp(s_current_path, cmd.path) == 0) {
                    /* 同一首歌: 纯续播, 不清状态不再解析 */
                    s_pending_pcm = false;
                    s_state = STATE_PLAYING;
                    printf("[音频] 恢复播放\n");
                } else {
                    start_play(cmd.path);   /* 换歌则重新开播 */
                }
                break;
            case AUDIO_CMD_STOP:
                close_decoder();
                atomic_store_bool(&g_song_info_valid, false);
                s_state = STATE_IDLE;
                break;
            case AUDIO_CMD_SEEK:
                if (s_decoder && s_decoder->seek) {
                    uint32_t fsz = s_decoder->get_file_size(s_decoder);
                    uint32_t target = (uint64_t)fsz * cmd.param / 1000;
                    s_decoder->seek(s_decoder, target);
                    if (s_info_mux) xSemaphoreTake(s_info_mux, portMAX_DELAY);
                    g_song_info.elapsed_sec =
                        (uint64_t)g_song_info.duration_sec * cmd.param / 1000;
                    if (s_info_mux) xSemaphoreGive(s_info_mux);
                }
                break;
            case AUDIO_CMD_PAUSE:
            case AUDIO_CMD_BT_CONNECTED:
            case AUDIO_CMD_BT_DISCONNECTED:
            default:
                break;
            }
            break;
        }
    }
}

extern "C" void audio_task_init(const audio_task_params_t *params)
{
    s_cmd_queue  = params->cmd_queue;
    s_rsp_queue  = params->rsp_queue;
    s_pcm_stream = params->pcm_stream;

    if (!s_info_mux) s_info_mux = xSemaphoreCreateMutex();   /* 歌曲信息快照锁 (须在任务启动前建好) */

    if (!astream_init()) {   /* SD 预读缓冲 (20KB PSRAM) */
        printf("[音频] FATAL: 预读缓冲分配失败\n");
        return;
    }

    /* 大缓冲优先放 PSRAM, 失败回退内部 RAM (纯 CPU 顺序访问, PSRAM 带宽绰绰有余) */
    s_pcm_buf = (int16_t *)heap_caps_malloc(MP3_PCM_BUF_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_pcm_buf) {
        s_pcm_buf = (int16_t *)heap_caps_malloc(MP3_PCM_BUF_SAMPLES * sizeof(int16_t), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    }
    s_out_buf = (int16_t *)heap_caps_malloc(PCM_OUT_BUF_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_out_buf) {
        s_out_buf = (int16_t *)heap_caps_malloc(PCM_OUT_BUF_SAMPLES * sizeof(int16_t), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    }
    if (!s_pcm_buf || !s_out_buf) {
        printf("[音频] FATAL: PCM/OUT 缓冲分配失败\n");
        return;
    }
    xTaskCreatePinnedToCore(audio_task, "audio", 4096, NULL, 1, &s_task, 0);
}

/* 挂起音频任务 (模式切到小说): 等任务就绪后再挂, 避免挂到未初始化的任务上.
 * 调用前须确保解码器已关闭 (g_audio_decoder_open==false), 否则会把持 SD 的任务挂死. */
extern "C" void audio_task_pause(void)
{
    if (!s_task) return;
    for (int i = 0; i < 200 && !s_task_ready; i++) {   /* 最多等 ~1s */
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (!s_task_ready) return;
    vTaskSuspend(s_task);
}

/* 恢复音频任务 (模式切回音乐) */
extern "C" void audio_task_resume(void)
{
    if (s_task) vTaskResume(s_task);
}
