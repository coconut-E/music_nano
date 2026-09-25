#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "nvs_flash.h"
#include "app.h"
#include "drv_display.h"
#include "power_mgr.h"
#include "ui_core.h"
#include "audio_task.h"
#include "bt_a2dp.h"
#include "sys_monitor.h"
#include "cover.h"
#include "likes.h"
#include "app_mode.h"

/* 系统级消息队列句柄 (跨模块共享) */
static QueueHandle_t s_app_cmd_queue  = NULL;   /* 应用命令队列: console→app 层(扫描/连接/播放等) */
static QueueHandle_t s_audio_cmd_queue = NULL;  /* 音频命令队列: UI→音频任务(播放/停止/音量等) */
static QueueHandle_t s_audio_rsp_queue = NULL;  /* 音频应答队列: 音频任务→UI(当前歌曲/状态回调) */

/* 系统入口: 初始化各子系统并启动对应任务.
 * 所有任务由子系统 init 内部创建, 本函数最后把自己挂起, 只保留各任务在跑. */
void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(80));//I (53) HOOK: RST released (GPIO25)->I (92) main_task: Calling app_main() only 39ms but The LCD needs at least 120ms stabilization time after power-on or reset, hence an 80ms delay here.
    /* 最先: 开外设供电 + LCD 前半段 (SPI/面板/SLPOUT, 非阻塞), 让 120ms 在启动期间流逝.
     * 这样把 LCD 上电时序 "藏" 进后续初始化时间里, 减少用户可见的启动延迟. */
    power_mgr_early_init();
    lcd_init_early(SPI2_HOST);

    /* 开机低电量检测: 初始化 ADC + 3 次采样平均 → g_vbat.
     * 电压 < 3.35V 时: 显示低电量图 2 秒后进入深度睡眠 (此处不返回);
     * 电压正常: 直接返回, 继续正常启动 (lcd_init_finish 在 LVGL 任务完成). */
    power_mgr_boot_battery_check();

    nvs_flash_init();   /* 初始化非易失存储 (保存配对信息/亮度/音量等设置) */

    /* 读取上次睡眠时的应用模式, 供 UI 决定启动构建哪一组 (只建一组, 加快启动) */
    app_mode_t boot_mode = app_mode_read_saved();
    app_mode_set_boot_mode(boot_mode);

    likes_init();       /* 载入喜欢列表到 RAM (文件浏览器显示爱心图标用) */

    /* 创建三个系统队列 (容量 10/10/5, 元素为对应命令结构体) */
    s_app_cmd_queue  = xQueueCreate(10, sizeof(app_cmd_t));    /* 应用命令 */
    s_audio_cmd_queue = xQueueCreate(10, sizeof(audio_cmd_t)); /* 音频命令 */
    s_audio_rsp_queue = xQueueCreate(5,  sizeof(audio_rsp_t)); /* 音频应答 */

    /* 初始化蓝牙 A2DP 子系统; 返回的接口给 UI/音频共用 */
    bt_a2dp_iface_t *bt_iface = bt_a2dp_init();
    if (!bt_iface) {
        printf("FATAL: 蓝牙初始化失败\n");
        return;
    }

    /* 串口控制台: 把解析出的命令发给 app 队列 */
    console_init(s_app_cmd_queue);

    /* UI 子系统参数: 命令/应答队列 + 蓝牙接口 */
    ui_params_t ui_params = {
        .app_cmd_queue   = s_app_cmd_queue,
        .bt_iface        = bt_iface,
        .audio_cmd_queue = s_audio_cmd_queue,
        .audio_rsp_queue = s_audio_rsp_queue,
    };
    ui_core_init(&ui_params);

    /* 音频任务参数: 命令/应答队列 + 蓝牙 PCM 流接口 */
    audio_task_params_t audio_params = {
        .cmd_queue  = s_audio_cmd_queue,
        .rsp_queue  = s_audio_rsp_queue,
        .pcm_stream = bt_iface->pcm_stream,
    };
    audio_task_init(&audio_params);

    /* 系统监视任务: 电池电压/CPU温度采样 + SD卡检测 */
    sys_monitor_init();

    /* 封面解码任务: 解析内嵌专辑封面 (经 app 命令队列通知) */
    cover_init(s_app_cmd_queue);

    /* 启动即小说模式: 等音频/封面任务初始化完成后挂起 (省电, 让出 core1).
     * 切回音乐模式时由 app_mode 恢复; 不切回则一直挂起 */
    if (boot_mode == APP_MODE_NOVEL) {
        audio_task_pause();
        cover_task_pause();
        bt_a2dp_pause();
    }

    printf("系统就绪\n");

    vTaskSuspend(NULL);   /* 入口任务不再需要, 永久挂起*/
}
