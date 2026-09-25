#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "bt_a2dp.h"
#include "audio_task.h"
#include "sys_monitor.h"
#include "app.h"
#include "drv_display.h"
#include "ui_core.h"
#include "ui_player.h"
#include "ui_novel.h"
#include "ui_shell.h"
#include "menu.h"
#include "app_mode.h"
#include "power_mgr.h"

#define LVGL_TAG "LVGL"

static QueueSetHandle_t s_queue_set = NULL;   /* 队列集: 聚合三个输入队列 */

/* LVGL 主任务: 跑 LVGL 渲染循环 + 分发 app/蓝牙/音频事件 */
void ui_loop_task(void *arg)
{
    /* 初始化显示/触摸驱动, 建 UI 各界面 */
    ui_core_display_init();

    ui_shell_init();                     /* 通用 UI 运行时 (弹窗/按键/SD监视) */
    bt_list_init(g_ui_bt_iface);         /* 蓝牙列表 */
    fs_list_set_play_cb(player_play_file);    /* 文件列表(音乐)点击 → 播放回调 */
    fs_list_set_novel_cb(ui_novel_open);      /* 文件列表(小说)点击 → 阅读回调 */
    app_mode_init();                     /* 按启动模式只构建并显示对应组 UI */

    /* 三个事件源聚合到一个队列集, 统一非阻塞轮询 */
    s_queue_set = xQueueCreateSet(3);
    xQueueAddToSet(g_ui_app_cmd_queue,  s_queue_set);         /* 应用命令 */
    xQueueAddToSet(g_ui_bt_iface->evt_queue, s_queue_set);    /* 蓝牙事件 */
    xQueueAddToSet(g_ui_audio_rsp_queue, s_queue_set);        /* 音频应答 */

    /* LCD 后半段: 距 SLPOUT ≥120ms 后发寄存器命令 + DISPON (不足则阻塞补齐) */
    lcd_init_finish();
    lv_timer_handler();   /* 跑一帧, 让界面先画出来 */
    power_mgr_init();     /* 电源管理 (背光渐入/按键) */
    /* 文件浏览器不再一次性阻塞创建 (~100ms), 改为下面的主循环里按时间预算分片完成:
     * 背光渐入动画约 300ms, 每轮只占用一小片, 卡顿被渐入掩盖 */
    //ESP_LOGI("LVGL", "预建文件浏览器 (分步)"); 

    app_cmd_t   app_cmd;      /* 应用命令 */
    bt_evt_t    bt_evt;       /* 蓝牙事件 */
    audio_rsp_t audio_rsp;    /* 音频应答 */
    audio_cmd_t audio_cmd;    /* 音频命令 (转发用) */
    bt_cmd_t    bt_cmd;       /* 蓝牙命令 (转发用) */

    ESP_LOGI("LVGL", "LVGL is OK"); 

    while (1) {
        lv_timer_handler();   /* LVGL 渲染/动画/定时器 */

        /* 分步预建文件浏览器: 每轮最多 10ms, 建完返回 false 后调用几乎零开销.
         * 放在渲染之后, 让背光渐入动画每轮都先推进一帧, 再吃掉这一小片 CPU */
        fs_browser_precreate_step(10000);

        QueueHandle_t active = xQueueSelectFromSet(s_queue_set, 0);   /* 查是否有事件 */

        /* ── 应用命令 (串口控制台) ── */
        if (active == g_ui_app_cmd_queue) {
            while (xQueueReceive(g_ui_app_cmd_queue, &app_cmd, 0) == pdTRUE) {
                /* 小说模式: 禁止音乐/蓝牙相关命令, 串口提示后丢弃 */
                if (app_mode_is_novel() &&
                    (app_cmd.type == APP_CMD_BT_SCAN ||
                     app_cmd.type == APP_CMD_BT_CONNECT ||
                     app_cmd.type == APP_CMD_BT_DISCONNECT ||
                     app_cmd.type == APP_CMD_PLAY ||
                     app_cmd.type == APP_CMD_STOP ||
                     app_cmd.type == APP_CMD_PAUSE)) {
                    printf("[LVGL] 小说模式: 禁止命令 (type=%d)\n", app_cmd.type);
                    continue;
                }

                switch (app_cmd.type) {
                case APP_CMD_BT_SCAN:   /* 转发蓝牙扫描命令 */
                    memset(&bt_cmd, 0, sizeof(bt_cmd));
                    bt_cmd.type = BT_CMD_SCAN;
                    xQueueSend(g_ui_bt_iface->cmd_queue, &bt_cmd, 0);
                    printf("[LVGL] scan\n");
                    break;

                case APP_CMD_BT_CONNECT:   /* 转发蓝牙连接命令 */
                    if (bt_a2dp_is_connected()) {
                        printf("[LVGL] already connected\n");
                    } else {
                        memset(&bt_cmd, 0, sizeof(bt_cmd));
                        bt_cmd.type = BT_CMD_CONNECT;
                        size_t plen = strnlen(app_cmd.param, sizeof(bt_cmd.device_name) - 1);
                        memcpy(bt_cmd.device_name, app_cmd.param, plen);
                        bt_cmd.device_name[plen] = '\0';
                        xQueueSend(g_ui_bt_iface->cmd_queue, &bt_cmd, 0);
                        printf("[LVGL] connecting: %s\n", app_cmd.param);
                    }
                    break;

                case APP_CMD_BT_DISCONNECT:   /* 转发断开命令 */
                    memset(&bt_cmd, 0, sizeof(bt_cmd));
                    bt_cmd.type = BT_CMD_DISCONNECT;
                    xQueueSend(g_ui_bt_iface->cmd_queue, &bt_cmd, 0);
                    printf("[LVGL] disconnecting\n");
                    break;

                case APP_CMD_PLAY:   /* 播放固定测试曲 (已连蓝牙才可) */
                    if (bt_a2dp_is_connected()) {
                        struct stat st;
                        if (!sdmmc_disk_is_mounted()) {
                            printf("[LVGL] SD not mounted\n");
                        } else if (stat("/sdcard/a.mp3", &st) != 0) {
                            printf("[LVGL] /sdcard/a.mp3 not found\n");
                        } else {
                            audio_cmd.type = AUDIO_CMD_PLAY;
                            strcpy(audio_cmd.path, "/sdcard/a.mp3");
                            xQueueSend(g_ui_audio_cmd_queue, &audio_cmd, 0);
                            player_set_was_playing(true);
                            printf("[LVGL] PLAY sent | ts=%lld us\n", esp_timer_get_time());
                        }
                    } else {
                        printf("[LVGL] not connected, can't play\n");
                    }
                    break;

                case APP_CMD_STOP:   /* 停止 */
                    audio_cmd.type = AUDIO_CMD_STOP;
                    xQueueSend(g_ui_audio_cmd_queue, &audio_cmd, 0);
                    player_set_was_playing(false);
                    break;

                case APP_CMD_PAUSE:   /* 暂停 */
                    audio_cmd.type = AUDIO_CMD_PAUSE;
                    xQueueSend(g_ui_audio_cmd_queue, &audio_cmd, 0);
                    break;

                case APP_CMD_INFO:   /* 打印状态 */
                    printf("[info] connected: %s | playing: %s\n",
                           bt_a2dp_is_connected() ? "yes" : "no",
                           player_was_playing() ? "yes" : "no");
                    break;

                case APP_CMD_COVER_READY: {   /* 封面解码完成 */
                    void *buf;
                    memcpy(&buf, app_cmd.param, sizeof(buf));   /* 取出封面缓冲指针 */
                    player_show_cover(buf);
                    break;
                }
                }
            }
        }

        /* ── 蓝牙事件 ── */
        if (active == g_ui_bt_iface->evt_queue) {
            while (xQueueReceive(g_ui_bt_iface->evt_queue, &bt_evt, 0) == pdTRUE) {
                switch (bt_evt.type) {
                case BT_EVT_DEVICE_FOUND:   /* 扫描到设备 → 列表 */
                    bt_list_on_device_found(bt_evt.device_name);
                    break;
                case BT_EVT_SCAN_DONE:
                    bt_list_on_scan_done();
                    break;
                case BT_EVT_CONNECTED:   /* 连接成功 → 通知音频准备推流 */
                    audio_cmd.type = AUDIO_CMD_BT_CONNECTED;
                    xQueueSend(g_ui_audio_cmd_queue, &audio_cmd, 0);
                    bt_list_on_connected(bt_evt.device_name);
                    if (player_was_playing()) {
                        printf("[LVGL] BT resume, notify audio\n");
                    }
                    break;
                case BT_EVT_CONNECT_FAILED:
                    bt_list_on_connect_failed(bt_evt.device_name);
                    break;
                case BT_EVT_DISCONNECTED:   /* 断开 → 通知音频停推 */
                    audio_cmd.type = AUDIO_CMD_BT_DISCONNECTED;
                    xQueueSend(g_ui_audio_cmd_queue, &audio_cmd, 0);
                    bt_list_on_disconnected();
                    break;
                case BT_EVT_STREAM_READY:
                    printf("[stream ready]\n");
                    break;
                case BT_EVT_STREAM_STOPPED:
                    printf("[stream stopped]\n");
                    break;
                case BT_EVT_PLAY_PAUSE:   /* 耳机播放/暂停键 */
                    printf("[LVGL] BT 切换播放/暂停\n");
                    player_toggle_play();
                    break;
                case BT_EVT_TRANSPORT_NEXT:   /* 耳机下一曲 */
                    printf("[LVGL] BT 下一曲\n");
                    player_next();
                    break;
                case BT_EVT_TRANSPORT_PREV:   /* 耳机上一曲 */
                    printf("[LVGL] BT 上一曲\n");
                    player_prev();
                    break;
                case BT_EVT_STATE_RSP:   /* 蓝牙状态应答 */
                    bt_list_on_state_rsp(bt_evt.state, bt_evt.device_name);
                    break;
                }
            }
        }

        /* ── 音频应答 ── */
        if (active == g_ui_audio_rsp_queue) {
            while (xQueueReceive(g_ui_audio_rsp_queue, &audio_rsp, 0) == pdTRUE) {
                switch (audio_rsp.type) {
                case AUDIO_RSP_BT_CHECK:   /* 核对蓝牙状态并回发音频 */
                    if (bt_a2dp_is_connected()) {
                        audio_cmd.type = AUDIO_CMD_BT_CONNECTED;
                    } else {
                        audio_cmd.type = AUDIO_CMD_BT_DISCONNECTED;
                    }
                    xQueueSend(g_ui_audio_cmd_queue, &audio_cmd, 0);
                    break;
                case AUDIO_RSP_FILE_NOT_FOUND:   /* 文件不存在 */
                    player_on_file_not_found();
                    break;
                case AUDIO_RSP_SONG_FINISHED:   /* 一首播放完 → 自动下一首 */
                    player_on_song_finished();
                    break;
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(2));   /* 让出 CPU 2ms */
    }
}
