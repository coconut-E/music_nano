#include "resampler_fxp.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#else
#define heap_caps_malloc(s, c) malloc(s)
#define heap_caps_free(p) free(p)
#endif

/*
 * 定点有理比率 polyphase 重采样, 目标 44100Hz。
 *
 * 原理: 输出样本 n 对应输入位置 n*(q/p) (输入采样为单位), 分解为
 *   ipos = floor(n*q/p), frac = (n*q mod p)/p
 * 输出 y[n] = Σ_j x[ipos - (j - half + 1)] * h[frac][j]
 * 其中 h[phase][j] = 窗函数化的 sinc 低通在 u = frac + (j - half + 1) 处取值,
 * 每个相位归一化到 Q14 (满量程 16384, DC 增益 1)。整数实现, int32 累加。
 */

typedef struct resampler_fxp_s {
    uint32_t  src_rate;    /* 源采样率 */
    uint8_t   channels;    /* 声道数 1/2 */
    int32_t   taps;        /* 每相位抽头数 (16 或 32, 2 的幂) */
    int32_t   half;        /* taps/2 */
    int32_t   p, q;        /* 比率 p/q = 44100/有效采样率 (约分后) */
    int32_t   decim;       /* 输入整数降采样因子 (如 96k→48k 为 2; 1=不降) */
    int32_t   pos;         /* 相位累加 0..p-1 (frac = pos/p) */
    int32_t   ipos;        /* 当前输出对应的输入样本绝对位置 */
    int32_t   in_next;     /* 已读入输入样本数 (历史窗口上界, 绝对) */
    int16_t  *coeff;       /* [p][taps] Q14 系数 (PSRAM, 只读) */
    int16_t   hist[2][32]; /* 环形历史 (内部 RAM, 常驻, 每输出都访问) */
    int16_t   rowbuf[32];  /* 当前相位系数行 staging (内部 RAM, 避免逐抽头 PSRAM 缺失) */
} resampler_fxp_t;

/* 最大公约数 */
static int32_t gcd_int(int32_t a, int32_t b)
{
    while (b) {
        int32_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

resampler_fxp_t *resampler_fxp_create(void)
{
    return (resampler_fxp_t *)calloc(1, sizeof(resampler_fxp_t));
}

/* 配置重采样器: src_rate=源采样率, channels=声道数.
 * 在 PSRAM 生成 [p 相位 × taps 抽头] 的 sinc 低通系数. */
bool resampler_fxp_open(resampler_fxp_t *r, uint32_t src_rate, uint8_t channels)
{
    if (!r || src_rate == 0 || (channels != 1 && channels != 2)) return false;

    resampler_fxp_close(r);

    /* 整数降采样: 源采样率 ≥ 2 倍且能被 2 整除时先 /2, 让 polyphase 落在近 1:1
     * (16 抽头), 避免 96k/192k/88.2k 走 32 抽头重采样。
     * 88.2k → /2 → 44.1k 精确, 变纯抽取 (p=q=1); 96k → /2 → 48k。 */
    int32_t decim = 1;
    int32_t rate = (int32_t)src_rate;
    for (;;) {
        if ((rate & 1) || rate / 2 < 44100) break;      /* 奇数或降到 44.1k 以下 */
        int32_t g = gcd_int(44100, rate);
        int32_t pp = 44100 / g;
        int32_t qq = rate / g;
        if (qq < 2 * pp) break;                          /* 已近 1:1, 不再降 */
        decim *= 2;
        rate /= 2;
    }

    /* 比率约分: 44100 / 有效采样率 = p/q */
    int32_t p = 44100;
    int32_t q = rate;
    int32_t g = gcd_int(p, q);
    p /= g;
    q /= g;

    /* 下采样 >=2 倍时用 32 抽头防混叠, 近 1:1 用 16 抽头 */
    int32_t taps = (q >= 2 * p) ? 32 : 16;
    int32_t half = taps / 2;

    /* 低通截止频率 fc = min(0.5, p/(2q)), 以输入采样率为 1 */
    double fc = 0.5;
    if (p < q) fc = (double)p / (2.0 * q);

    int16_t *coeff = (int16_t *)heap_caps_malloc((size_t)p * taps * sizeof(int16_t),
                                                 MALLOC_CAP_SPIRAM);   /* 系数表放 PSRAM */
    if (!coeff) return false;

    /* 生成每个相位的窗函数化 sinc 系数 (先求总和做 DC 归一化) */
    for (int32_t ph = 0; ph < p; ph++) {
        double frac = (double)ph / p;   /* 该相位相对位置 0..1 */
        double sum = 0.0;
        int16_t *row = coeff + (size_t)ph * taps;

        for (int32_t j = 0; j < taps; j++) {
            double u = frac + (double)(j - half + 1);
            double s;
            if (u == 0.0) {
                s = 1.0;   /* sinc(0) */
            } else {
                double x = 2.0 * M_PI * fc * u;
                s = sin(x) / x;   /* 理想低通冲激响应 */
            }
            double w = 0.5 + 0.5 * cos(M_PI * u / half);   /* 汉宁窗 */
            sum += s * w;
        }

        /* 每相位归一化到 Q14 (满量程 16384): int32 累加安全
         * (Σ|c| 约 ≤3.36e4, ×输入32767 ≈ 1.1e9 < 2^31), 避免 int64 carry 序列 */
        for (int32_t j = 0; j < taps; j++) {
            double u = frac + (double)(j - half + 1);
            double s;
            if (u == 0.0) {
                s = 1.0;
            } else {
                double x = 2.0 * M_PI * fc * u;
                s = sin(x) / x;
            }
            double w = 0.5 + 0.5 * cos(M_PI * u / half);
            long v = lrint(s * w / sum * 16384.0);   /* 归一化到 Q14 */
            if (v > 16384) v = 16384;
            if (v < -16384) v = -16384;
            row[j] = (int16_t)v;
        }
    }

    r->src_rate = src_rate;
    r->channels = channels;
    r->taps     = taps;
    r->half     = half;
    r->p        = p;
    r->q        = q;
    r->decim    = decim;
    r->pos      = 0;
    r->ipos     = 0;
    r->in_next  = 0;
    r->coeff    = coeff;
    memset(r->hist, 0, sizeof(r->hist));
    return true;
}

/* 重采样一块输入到输出.
 * src=交错 int16 输入, src_frames=输入帧数, dst=输出缓冲, dst_cap_frames=输出容量(帧).
 * 返回实际输出帧数. */
size_t resampler_fxp_process(resampler_fxp_t *r, const int16_t *src, size_t src_frames,
                             int16_t *dst, size_t dst_cap_frames)
{
    if (!r || !r->coeff || !src || !dst || src_frames == 0 || dst_cap_frames == 0) return 0;

    const int32_t taps = r->taps;
    const int32_t half = r->half;
    const int32_t mask = taps - 1;      /* taps 为 2 的幂 → 位与即取模 */
    const int32_t ch   = r->channels;
    const int32_t p    = r->p;
    const int32_t q    = r->q;
    int16_t *out = dst;
    size_t out_frames = 0;

    while (out_frames < dst_cap_frames && src_frames >= r->decim) {
        /* 确保历史缓冲里有 [ipos-half, ipos+half-1]; 每存 1 个样本消耗 decim 个输入帧 */
        while (r->ipos + half - 1 >= r->in_next && src_frames >= r->decim) {
            const int16_t *p = src + (r->decim - 1) * ch;   /* 降采样取第 decim 帧 */
            for (int32_t c = 0; c < ch; c++) {
                r->hist[c][r->in_next & mask] = p[c];   /* 写入环形历史 */
            }
            src += r->decim * ch;
            src_frames -= r->decim;
            r->in_next++;
        }
        if (r->ipos + half - 1 >= r->in_next) break;   /* 输入不足, 等待下块 */

        /* 产生一帧输出: 先把当前相位系数行拷贝到内部 rowbuf,
         * 避免 16 抽头循环逐次读 PSRAM (缓存缺失 ~40 周期/次) */
        const int16_t *cp = r->coeff + (size_t)r->pos * taps;
        memcpy(r->rowbuf, cp, (size_t)taps * sizeof(int16_t));
        const int16_t *rb = r->rowbuf;
        for (int32_t c = 0; c < ch; c++) {
            const int16_t *hp = r->hist[c];
            int32_t acc = 0;   /* Q14: int32 累加, 不溢出 (见 open 注释) */
            for (int32_t j = 0; j < taps; j++) {   /* polyphase 点积 */
                int32_t a = j - half + 1;
                acc += (int32_t)hp[(r->ipos - a) & mask] * rb[j];
            }
            *out++ = (int16_t)((acc + 8192) >> 14);   /* 舍入到 Q14 再右移回 int16 */
        }
        out_frames++;

        /* 前进到下一输出位置: pos += q (mod p), ipos 进位 */
        r->pos += q;
        while (r->pos >= p) {
            r->pos -= p;
            r->ipos++;   /* 每累计满一个相位周期, 输入位置前进 1 样本 */
        }
    }

    return out_frames;
}

void resampler_fxp_close(resampler_fxp_t *r)
{
    if (!r) return;
    if (r->coeff) {
        heap_caps_free(r->coeff);
        r->coeff = NULL;
    }
    r->p = 0;
    r->q = 0;
    r->pos = 0;
    r->ipos = 0;
    r->in_next = 0;
}

void resampler_fxp_free(resampler_fxp_t *r)
{
    if (!r) return;
    resampler_fxp_close(r);
    free(r);
}
