#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "settings.h"
#include "sys_monitor.h"
#include "audio_task.h"
#include "bt_a2dp.h"
#include "cover.h"
#include "atomic_utils.h"
#include "ui_core.h"
#include "ui_player.h"
#include "ui_novel.h"
#include "ui_shell.h"
#include "menu.h"
#include "power_mgr.h"
#include "app_mode.h"

static const char *TAG = "APP_MODE";

static app_mode_t s_mode      = APP_MODE_MUSIC;   /* 当前模式 */
static app_mode_t s_boot_mode = APP_MODE_MUSIC;   /* 启动模式 (main.c 设置) */
static bool       s_switching = false;            /* 切换中 (防重入) */

void app_mode_set_boot_mode(app_mode_t m)
{
    s_boot_mode = m;
}

app_mode_t app_mode_read_saved(void)
{
    return (settings_app_mode_load() == APP_MODE_NOVEL) ? APP_MODE_NOVEL : APP_MODE_MUSIC;
}

app_mode_t app_mode_get(void)
{
    return s_mode;
}

bool app_mode_is_novel(void)
{
    return s_mode == APP_MODE_NOVEL;
}

/* 等待音频解码器释放 SD (切换挂起前必须确认, 避免挂死持锁任务) */
static void wait_decoder_closed(void)
{
    for (int i = 0; i < 60 && atomic_load_bool(&g_audio_decoder_open); i++) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

/* 实际切换工作: 在 power_mgr 黑屏窗口内 (LVGL 任务上下文) 执行 */
static void app_mode_switch_work(void)
{
    if (s_mode == APP_MODE_MUSIC) {
        /* ── 音乐 → 小说 ── */
        player_stop_playback();
        wait_decoder_closed();

        /* 挂起音频/封面/蓝牙任务 (省电; 封面任务在 core1, 蓝牙任务在 core0).
         * bt_a2dp_pause 内部停扫描+断连后 vTaskSuspend */
        audio_task_pause();
        cover_task_pause();
        bt_a2dp_pause();

        player_destroy();

        fs_browser_close_panel();
        fs_browser_set_source(APP_MODE_NOVEL);
        ui_novel_show();

        s_mode = APP_MODE_NOVEL;
    } else {
        /* ── 小说 → 音乐 ── */
        ui_novel_destroy();

        audio_task_resume();
        cover_task_resume();
        bt_a2dp_resume();

        fs_browser_close_panel();
        fs_browser_set_source(APP_MODE_MUSIC);
        player_show();
        player_on_sd_ready();   /* 切回音乐: 恢复上次播放歌曲 (只加载不自动播放) */

        s_mode = APP_MODE_MUSIC;
    }

    ui_shell_on_mode_changed();
    settings_app_mode_save((int)s_mode);
    ESP_LOGI(TAG, "模式已切换: %s", s_mode == APP_MODE_NOVEL ? "小说" : "音乐");
    s_switching = false;
}

void app_mode_toggle(void)
{
    if (s_switching) return;
    s_switching = true;
    power_mgr_mode_transition(app_mode_switch_work);
}

void app_mode_init(void)
{
    s_mode = s_boot_mode;
    fs_browser_set_source(s_mode);

    if (s_mode == APP_MODE_NOVEL) {
        ui_novel_show();
    } else {
        player_show();
    }
    ui_shell_on_mode_changed();
    ESP_LOGI(TAG, "启动模式: %s", s_mode == APP_MODE_NOVEL ? "小说" : "音乐");
}
