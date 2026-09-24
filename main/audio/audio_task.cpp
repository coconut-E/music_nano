#include <cstdio>
#include <cstring>
#include <cinttypes>
#include <cstdlib>
#include <strings.h>
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

static audio_decoder_t     *s_decoder     = NULL;   /* 当前解码器对象 */
static char                 s_current_path[256];    /* 当前播放文件路径 */

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
        sd_fs_lock();                  /* 与 SD 卸载互斥: 避免 fclose 与卸载竞争 */
        d->close(d);                   /* 各解码器的资源释放 (fclose) */
        atomic_store_bool(&g_audio_decoder_open, false);   /* 锁内置否, 卸载等待可立即看到 */
        sd_fs_unlock();
        free(d);                       /* 释放对象本体 */
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
    sd_fs_lock();   /* 与卸载互斥; 持锁后再复核 SD 是否仍就绪 */
    bool opened = atomic_load_bool(&g_sd_ready) && s_decoder->open(s_decoder, path);
    if (opened) atomic_store_bool(&g_audio_decoder_open, true);   /* 锁内公示, 供卸载等待 */
    sd_fs_unlock();
    if (!opened) {
        printf("[音频] 无法打开 %s\n", path);
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
                        sd_fs_lock();
                        bool ok = s_decoder->decode(s_decoder, s_pcm_buf, &bytes) && bytes > 0;
                        sd_fs_unlock();
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
                        sd_fs_lock();
                        s_decoder->seek(s_decoder, target);
                        sd_fs_unlock();
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
                sd_fs_lock();   /* 与 SD 卸载互斥 */
                bool dec_ok = s_decoder->decode(s_decoder, s_pcm_buf, &s_pcm_bytes);
                sd_fs_unlock();
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

                /* 转换到 44.1k stereo 输出缓冲 */
                size_t frames = s_pcm_bytes / (ch * 2);   /* 帧数 = 字节/(声道数×2字节/样本) */
                s_out_bytes = pcm_pipeline_process(s_pipeline, s_pcm_buf, frames,
                                                   (uint8_t*)s_out_buf, PCM_OUT_BUF_SAMPLES * sizeof(int16_t));

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
                    sd_fs_lock();
                    s_decoder->seek(s_decoder, target);
                    sd_fs_unlock();
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
    xTaskCreatePinnedToCore(audio_task, "audio", 4096, NULL, 1, NULL, 0);
}
