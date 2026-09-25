#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

void cover_init(QueueHandle_t app_cmd_queue);   /* 启动封面解码任务: app_cmd_queue=完成通知队列 */
/* 提交封面解码作业: jpg=JPEG数据 (所有权移交), size=数据大小.
 * 返回 true=已接管所有权; false=未接管 (调用方仍持有, 需自行释放) */
bool cover_submit_job(const uint8_t *jpg, size_t size);

/* 通知 UI: 当前歌曲无内嵌封面, 回退默认图标 */
void cover_notify_no_cover(void);

/* 模式切换挂起/恢复: 切到小说时挂起 (封面色块任务在 core1, 挂起以让出 CPU) */
void cover_task_pause(void);
void cover_task_resume(void);

#ifdef __cplusplus
}
#endif
