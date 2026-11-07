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
#include "ui_menu.h"
#include "ui_shell.h"
#include "menu.h"
#include "power_mgr.h"
#include "app_mode.h"

static const char *TAG = "APP_MODE";

static app_mode_t s_mode      = APP_MODE_MUSIC;   /* 当前模式 */
static app_mode_t s_boot_mode = APP_MODE_MUSIC;   /* 启动模式 (main.c 设置) */
static app_mode_t s_pending   = APP_MODE_MUSIC;   /* 本次要切到的目标模式 */
static bool       s_switching = false;            /* 切换中 (防重入) */

static const char *mode_name(app_mode_t m)
{
    switch (m) {
    case APP_MODE_NOVEL:    return "小说";
    case APP_MODE_SETTINGS: return "设置";
    case APP_MODE_MENU:     return "菜单";
    default:                return "音乐";
    }
}

void app_mode_set_boot_mode(app_mode_t m)
{
    s_boot_mode = m;
}

app_mode_t app_mode_read_saved(void)
{
    /* 只认 0/1; NVS 里若出现 2(设置)/3(菜单) 一律回落到音乐 —— 这两者永不落盘 */
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

bool app_mode_is_menu(void)
{
    return s_mode == APP_MODE_MENU;
}

/* 等待音频解码器释放 SD (切换挂起前必须确认, 避免挂死持锁任务) */
static void wait_decoder_closed(void)
{
    for (int i = 0; i < 60 && atomic_load_bool(&g_audio_decoder_open); i++) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

/* ── 各模式的"退出/进入"动作 ──
 * 下面四个函数体是从原来的 music→novel / novel→music 两条分支里逐行搬出来的,
 * 逻辑一字未改: 只把"销毁旧的"和"建立新的"拆开, 好让菜单页也能复用同一套. */

/* 退出音乐: 停播 → 等解码器放开 SD → 挂起音频/封面/蓝牙 → 销毁播放器 UI */
static void mode_music_leave(void)
{
    player_stop_playback();
    wait_decoder_closed();

    /* 挂起音频/封面/蓝牙任务 (省电; 封面任务在 core1, 蓝牙任务在 core0).
     * bt_a2dp_pause 内部停扫描+断连后 vTaskSuspend */
    audio_task_pause();
    cover_task_pause();
    bt_a2dp_pause();

    player_destroy();

    fs_browser_close_panel();
}

/* 进入音乐: 恢复音频/封面/蓝牙 → 文件浏览器切回音乐库 → 重建播放器 UI */
static void mode_music_enter(void)
{
    audio_task_resume();
    cover_task_resume();
    bt_a2dp_resume();

    fs_browser_close_panel();
    fs_browser_set_source(APP_MODE_MUSIC);
    player_show();
    player_on_sd_ready();   /* 切回音乐: 恢复上次播放歌曲 (只加载不自动播放) */
}

/* 进入小说: 文件浏览器切到小说库 → 构建阅读界面 */
static void mode_novel_enter(void)
{
    fs_browser_close_panel();
    fs_browser_set_source(APP_MODE_NOVEL);
    ui_novel_show();
}

/* 退出小说: 销毁阅读界面 */
static void mode_novel_leave(void)
{
    ui_novel_destroy();
}

/* 实际切换工作: 在 power_mgr 黑场窗口内 (LVGL 任务上下文) 执行 */
static void app_mode_switch_work(void)
{
    /* 1) 退出当前模式 (只清理该模式自己持有的东西) */
    switch (s_mode) {
    case APP_MODE_MUSIC:    mode_music_leave(); break;
    case APP_MODE_NOVEL:    mode_novel_leave(); break;
    case APP_MODE_SETTINGS: break;               /* 占位: 无资源可清 */
    case APP_MODE_MENU:     ui_menu_destroy();  break;
    }

    /* 2) 进入目标模式 */
    switch (s_pending) {
    case APP_MODE_MUSIC:    mode_music_enter(); break;
    case APP_MODE_NOVEL:    mode_novel_enter(); break;
    case APP_MODE_SETTINGS: break;               /* TODO: 设置页, 先留空 */
    case APP_MODE_MENU:     ui_menu_build();    break;
    }

    s_mode = s_pending;

    /* 3) 唯一写盘点: 只有音乐/小说进 NVS 作为下次启动模式.
     *    设置和菜单永不落盘 —— 停在菜单时断电, 下次启动仍是上次的音乐/小说. */
    if (s_mode == APP_MODE_MUSIC || s_mode == APP_MODE_NOVEL) {
        settings_app_mode_save((int)s_mode);
    }

    ui_shell_on_mode_changed();
    ESP_LOGI(TAG, "模式已切换: %s", mode_name(s_mode));
    s_switching = false;
}

void app_mode_request(app_mode_t target)
{
    if (s_switching) return;
    if (target == s_mode) return;   /* 已在该模式: 什么都不做 (菜单里再长按即无反应) */
    s_switching = true;
    s_pending   = target;
    power_mgr_mode_transition(app_mode_switch_work);
}

void app_mode_init(void)
{
    s_mode    = s_boot_mode;
    s_pending = s_mode;
    fs_browser_set_source(s_mode);

    if (s_mode == APP_MODE_NOVEL) {
        ui_novel_show();
    } else {
        player_show();
    }
    ui_shell_on_mode_changed();
    ESP_LOGI(TAG, "启动模式: %s", mode_name(s_mode));
}
