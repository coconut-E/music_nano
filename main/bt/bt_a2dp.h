#ifndef __BT_A2DP_H__
#define __BT_A2DP_H__

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BT_CMD_SCAN       = 0,   /* 开始扫描设备 */
    BT_CMD_CONNECT    = 1,   /* 连接设备 (device_name=设备名) */
    BT_CMD_DISCONNECT = 2,   /* 断开当前连接 */
    BT_CMD_STOP_SCAN  = 3,   /* 停止扫描 */
    BT_CMD_GET_STATE  = 4,   /* 查询状态 (回 BT_EVT_STATE_RSP) */
} bt_cmd_type_t;

typedef struct {
    bt_cmd_type_t type;            /* 命令类型 */
    char          device_name[32]; /* 目标设备名 (连接用) */
} bt_cmd_t;

typedef enum {
    BT_STATE_DISCONNECTED = 0,   /* 未连接 */
    BT_STATE_CONNECTING,         /* 连接中 */
    BT_STATE_CONNECTED,          /* 已连接 */
} bt_state_t;

typedef enum {
    BT_EVT_DEVICE_FOUND    = 0,   /* 扫描发现设备 (device_name=名字) */
    BT_EVT_SCAN_DONE       = 1,   /* 扫描完成 */
    BT_EVT_CONNECTED       = 2,   /* 连接成功 */
    BT_EVT_CONNECT_FAILED  = 3,   /* 连接失败 (error=错误码) */
    BT_EVT_DISCONNECTED    = 4,   /* 已断开 */
    BT_EVT_STREAM_READY    = 5,   /* 音频流就绪 (可开始推 PCM) */
    BT_EVT_STREAM_STOPPED  = 6,   /* 音频流停止 */
    BT_EVT_STATE_RSP       = 7,   /* 状态查询应答 (state=当前状态) */
    BT_EVT_PLAY_PAUSE      = 8,  /* 耳机切换 播放/暂停 */
    BT_EVT_TRANSPORT_NEXT  = 9,  /* 耳机下一曲 */
    BT_EVT_TRANSPORT_PREV  = 10, /* 耳机上一曲 */
} bt_evt_type_t;

/* 蓝牙事件结构体 */
typedef struct {
    bt_evt_type_t type;          /* 事件类型 */
    bt_state_t    state;         /* 相关状态 (STATE_RSP 等用) */
    char          device_name[32]; /* 设备名 */
    int           error;         /* 错误码 */
} bt_evt_t;

/* 蓝牙 A2DP 对外接口 (命令/事件队列 + PCM 流缓冲) */
typedef struct {
    QueueHandle_t        cmd_queue;   /* 命令队列 (UI→蓝牙) */
    QueueHandle_t        evt_queue;   /* 事件队列 (蓝牙→UI) */
    StreamBufferHandle_t pcm_stream;  /* PCM 音频流缓冲 (音频任务写入, A2DP 读取) */
} bt_a2dp_iface_t;

bt_a2dp_iface_t *bt_a2dp_init(void);    /* 初始化蓝牙子系统, 返回接口 */

bt_state_t bt_a2dp_get_state(void);     /* 查询当前连接状态 */

bool bt_a2dp_is_connected(void);        /* 是否已连接 */

/* 模式切换挂起/恢复: 小说模式挂起蓝牙任务 (停扫描/断连后 vTaskSuspend) 省 CPU0;
 * 切回音乐恢复. 挂起期间 get_state/is_connected 均返回未连接 */
void bt_a2dp_pause(void);
void bt_a2dp_resume(void);

#ifdef __cplusplus
}
#endif

#endif
