#ifndef __PCM_PIPELINE_H__
#define __PCM_PIPELINE_H__

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 解码输出 -> 蓝牙推流 之间的中间转换层。
 *
 * 输入: 任意采样率 / 声道(1 或 2) / 位深(上混路径支持 8/16/24/32;
 *       直通路径假设 16bit —— 当前所有解码器均输出 16bit) 的交错 PCM
 * 输出: 固定 44.1kHz / 16bit / 双声道 交错 PCM
 *
 * 当源采样率 != 44100 时走 esp-audio-libs 的重采样器;
 * 当源为 mono 时先上混为 stereo; 源本身就是 44.1k/16bit/stereo 时
 * 直接 memcpy, 几乎零开销。
 */

typedef struct pcm_pipeline_s pcm_pipeline_t;

pcm_pipeline_t *pcm_pipeline_create(void);

/* 按源格式(重新)配置转换层。目标固定 44.1kHz/16bit/stereo。 */
bool pcm_pipeline_open(pcm_pipeline_t *p, uint32_t src_rate, uint8_t src_bits, uint8_t src_ch);

/*
 * 转换一块解码出的交错 PCM。
 * src: 交错样本起始, src_frames: 帧数(每帧含 src_ch 个样本)
 * dst: 输出缓冲(>= 输出字节数), dst_cap_bytes: 输出缓冲容量
 * 返回: 实际写入 dst 的字节数
 */
size_t pcm_pipeline_process(pcm_pipeline_t *p, const void *src, size_t src_frames,
                            uint8_t *dst, size_t dst_cap_bytes);

void pcm_pipeline_close(pcm_pipeline_t *p);
void pcm_pipeline_free(pcm_pipeline_t *p);

#ifdef __cplusplus
}
#endif

#endif
