#include "pcm_pipeline.h"
#include "impl/pcm_convert.h"
#include "resampler_fxp.h"

#include <cstring>
#include <cstdlib>
#include <cstdio>
#include "esp_timer.h"

using namespace esp_audio_libs;

#define TARGET_RATE   44100   /* 目标采样率 (蓝牙 A2DP) */
#define TARGET_BITS   16      /* 目标位深 */
#define TARGET_CH     2       /* 目标声道数 */

/* 最坏输入: MPEG1 stereo 1152 帧/块 */
#define MAX_IN_FRAMES  1152

/* 最低支持源采样率: 低于此值单块重采样输出会逼近/超出调用方缓冲, 保守拒绝.
 * (真实溢出边界 = 1152×44100/8192 ≈ 6202Hz, 取 8kHz 整数留余量) */
#define MIN_SRC_RATE   8000

/* 转换层内部结构 */
struct pcm_pipeline_s {
    resampler_fxp_t *res_fxp;   /* 重采样器 (仅需重采样时非 NULL) */

    bool     active;         /* 已配置可处理 */
    bool     need_resample;  /* 源采样率 != 44100 时需要重采样 */
    bool     need_upmix;     /* 源单声道时需要上混为立体声 */

    uint32_t src_rate;       /* 源采样率 */
    uint8_t  src_bits;       /* 源位深 */
    uint8_t  src_bps;        /* 源字节深 (位深/8) */
    uint8_t  src_ch;         /* 源声道数 */

    uint8_t *stereo_in;   /* mono -> stereo 上混临时缓冲 */

    int64_t  res_us;          /* 重采样累计耗时 (诊断) */
    int64_t  res_window_t0;   /* 统计窗口起点 */
    uint64_t res_frames;      /* 重采样累计输出帧数 (诊断) */
};

/* 创建转换层对象 */
pcm_pipeline_t *pcm_pipeline_create(void)
{
    pcm_pipeline_t *p = (pcm_pipeline_t *)calloc(1, sizeof(*p));
    if (!p) return NULL;

    /* 上混临时缓冲: 最多 1152 帧 × 双声道 × 4 字节/样本 (按最大位深 32bit 预留, 
     * 当前解码器均为 16bit, 实际仅用前 2 字节/样本) */
    p->stereo_in = (uint8_t *)malloc(MAX_IN_FRAMES * TARGET_CH * 4);
    if (!p->stereo_in) {
        free(p);
        return NULL;
    }
    return p;
}

/* 关闭当前配置 (释放重采样器), 对象仍可用 */
void pcm_pipeline_close(pcm_pipeline_t *p)
{
    if (!p) return;
    if (p->res_fxp) {
        resampler_fxp_free(p->res_fxp);
        p->res_fxp = NULL;
    }
    p->active = false;
    p->need_resample = false;
    p->need_upmix = false;
}

/* 释放整个对象 */
void pcm_pipeline_free(pcm_pipeline_t *p)
{
    if (!p) return;
    pcm_pipeline_close(p);
    free(p->stereo_in);
    p->stereo_in = NULL;
    free(p);
}

/* 按源格式(重新)配置转换层: src_rate=源采样率, src_bits=源位深, src_ch=源声道数.
 * 目标固定 44.1kHz/16bit/stereo. 成功返回 true. */
bool pcm_pipeline_open(pcm_pipeline_t *p, uint32_t src_rate, uint8_t src_bits, uint8_t src_ch)
{
    if (!p) return false;

    pcm_pipeline_close(p);   /* 先释放旧配置 */

    /* 参数合法性校验 (源率过低会使单块重采样输出超出缓冲, 拒绝) */
    if (src_rate < MIN_SRC_RATE || src_bits == 0 || (src_ch != 1 && src_ch != 2)) {
        return false;
    }

    p->src_rate = src_rate;
    p->src_bits = src_bits;
    p->src_bps  = src_bits / 8;
    p->src_ch   = src_ch;

    p->need_upmix    = (src_ch == 1);           /* 单声道要上混 */
    p->need_resample = (src_rate != TARGET_RATE);   /* 非 44.1k 要重采样 */

    if (p->need_resample) {
        p->res_fxp = resampler_fxp_create();    /* 创建纯整数重采样器 */
        if (!p->res_fxp || !resampler_fxp_open(p->res_fxp, src_rate, TARGET_CH)) {
            resampler_fxp_free(p->res_fxp);
            p->res_fxp = NULL;
            return false;
        }
    }

    p->active = true;
    p->res_us = 0;
    p->res_window_t0 = esp_timer_get_time();
    return true;
}

/* 对交错 int16 PCM 原地施加整数百分比增益 (0~100), 一次连续扫描.
 * 顺序访问, 对 PSRAM 友好; gain>=100 原样返回, gain<=0 直接静音. */
static void pcm_apply_gain_inplace(int16_t *s, size_t count, int gain)
{
    if (!s || count == 0 || gain >= 100) return;
    if (gain <= 0) {
        memset(s, 0, count * sizeof(int16_t));
        return;
    }
    for (size_t i = 0; i < count; i++) {
        s[i] = (int16_t)(((int32_t)s[i] * gain) / 100);
    }
}

/* 转换一块解码出的交错 PCM.
 * src=输入样本 (原地施加软件增益), src_frames=输入帧数, dst=输出缓冲,
 * dst_cap_bytes=输出容量, gain_percent=软件增益 (%).
 * 返回实际写入 dst 的字节数. */
size_t pcm_pipeline_process(pcm_pipeline_t *p, void *src, size_t src_frames,
                            uint8_t *dst, size_t dst_cap_bytes, uint8_t gain_percent)
{
    if (!p || !p->active || !src || !dst) return 0;

    uint8_t *in = (uint8_t *)src;

    /* 1) mono -> stereo: 用 pcm_convert 做格式拷贝+上混 */
    if (p->need_upmix) {
        pcm_convert::copy_frames(in, p->stereo_in,
                                 p->src_bps, p->src_ch,
                                 TARGET_BITS / 8, TARGET_CH,
                                 (uint32_t)src_frames);
        in = p->stereo_in;   /* 之后按立体声处理 */
    }

    /* 1.5) 软件音量层: 独立一层, 无论是否重采样都生效 (重采样层只在需重采样时走).
     *      对输入块原地缩放 —— 一次连续大块扫描, PSRAM 友好, 不新占缓冲. */
    uint8_t in_bps = p->need_upmix ? (TARGET_BITS / 8) : p->src_bps;
    if (in_bps == 2 && gain_percent != 100) {
        pcm_apply_gain_inplace((int16_t *)in, src_frames * TARGET_CH, gain_percent);
    }

    /* 2) 源速率 == 44100: 直接拷贝 (零开销) */
    if (!p->need_resample) {
        size_t bytes = src_frames * TARGET_CH * (TARGET_BITS / 8);
        if (bytes > dst_cap_bytes) bytes = dst_cap_bytes;
        memcpy(dst, in, bytes);
        return bytes;
    }

    /* 3) 纯整数重采样到 44.1kHz */
    size_t out_frames_cap = dst_cap_bytes / (TARGET_CH * (TARGET_BITS / 8));   /* 输出缓冲可容纳的帧数 */
    int64_t t0 = esp_timer_get_time();
    size_t out_frames = resampler_fxp_process(p->res_fxp,
                                              (const int16_t *)in, src_frames,
                                              (int16_t *)dst, out_frames_cap);
    p->res_us += esp_timer_get_time() - t0;
    p->res_frames += out_frames;

    /* 每 10s 汇报重采样 CPU 占比 (诊断) */
    int64_t now = esp_timer_get_time();
    if (now - p->res_window_t0 > 10 * 1000000) {
        int64_t win = now - p->res_window_t0;
        printf("[PIPE] 重采样 %d ms / %u 帧 / %.1f s (%.1f%% core, %.0f ns/帧, 入%u帧/次)\n",
               (int)(p->res_us / 1000), (unsigned)p->res_frames, win / 1000000.0,
               (double)p->res_us * 100.0 / win,
               (double)p->res_us * 1000.0 / (p->res_frames ? p->res_frames : 1),
               (unsigned)src_frames);
        p->res_us = 0;
        p->res_frames = 0;
        p->res_window_t0 = now;
    }

    return out_frames * TARGET_CH * (TARGET_BITS / 8);   /* 输出帧数 → 字节数 */
}
