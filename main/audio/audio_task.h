#ifndef __AUDIO_TASK_H__
#define __AUDIO_TASK_H__

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

extern volatile bool g_pcm_active;          /* 是否正在推送 PCM 到蓝牙 (全局标志) */
extern volatile bool g_audio_decoder_open;  /* 解码器是否已打开 (占用 SD 文件时为 true) */

/* 音频命令类型 (UI 通过 cmd_queue 发来) */
typedef enum {
    AUDIO_CMD_PLAY            = 0,   /* 开始播放 (path=文件路径) */
    AUDIO_CMD_STOP            = 1,   /* 停止播放 */
    AUDIO_CMD_PAUSE           = 2,   /* 暂停 */
    AUDIO_CMD_BT_CONNECTED    = 3,   /* 蓝牙已连接 (可开始推流) */
    AUDIO_CMD_BT_DISCONNECTED = 4,   /* 蓝牙断开 (停推) */
    AUDIO_CMD_SEEK            = 5,   /* 跳转 (param=千分比 0~1000, 非字节偏移) */
} audio_cmd_type_t;

/* 音频命令结构体: type=命令, path=播放路径, param=附加参数 */
typedef struct {
    audio_cmd_type_t type;
    char             path[256];
    uint32_t         param;
} audio_cmd_t;

/* 音频应答类型 (音频任务发给 UI 的通知) */
typedef enum {
    AUDIO_RSP_BT_CHECK       = 0,   /* 检查蓝牙连接状态 */
    AUDIO_RSP_FILE_NOT_FOUND = 1,   /* 文件不存在 */
    AUDIO_RSP_SONG_FINISHED  = 2,   /* 一首歌播放完毕 */
} audio_rsp_type_t;

typedef struct {
    audio_rsp_type_t type;   /* 应答类型 */
} audio_rsp_t;

/* 音频任务初始化参数 */
typedef struct {
    QueueHandle_t        cmd_queue;   /* 命令队列 (UI→音频) */
    QueueHandle_t        rsp_queue;   /* 应答队列 (音频→UI) */
    StreamBufferHandle_t pcm_stream;  /* 蓝牙 PCM 流缓冲 (解码后的音频写这里) */
} audio_task_params_t;

void audio_task_init(const audio_task_params_t *params);   /* 创建音频任务 */

/* 模式切换挂起/恢复: 切到小说时挂起 (省电/让出 CPU), 切回音乐时恢复.
 * pause 会等任务进入主循环后再挂; 调用方须先停止播放并确认解码器已关闭. */
void audio_task_pause(void);
void audio_task_resume(void);

#ifdef __cplusplus
}
#endif

#endif
