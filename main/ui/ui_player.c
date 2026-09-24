#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "drv_display.h"
#include "audio_task.h"
#include "audio.h"
#include "settings.h"
#include "sys_monitor.h"
#include "atomic_utils.h"
#include "ui_core.h"
#include "ui_player.h"
#include "menu.h"
#include "menu_like.h"
#include "menu_mode.h"
#include "power_mgr.h"
#include "played_bits.h"
#include "song_hash.h"
#include "likes.h"

extern const lv_font_t lv_font_global_16;

/* 音量按键: 高电平(外部下拉)即增减, 由 LVGL 独立定时器(10ms)轮询 */
#define PIN_VOL_UP     36
#define PIN_VOL_DOWN   38
#define PIN_PWR_KEY    37                /* 息屏/唤醒按键 (GPIO37, 外部10k下拉) */
#define VOLUME_STEP    4                  /* 单步步进 */
#define VOL_KEY_POLL_MS 10                /* 轮询周期 10ms */
#define VOL_LONGPRESS_MS 600              /* 长按判定: 超过此值进入重复模式 */
#define VOL_REPEAT_MS  150                /* 长按重复步进间隔 */

/* 音量弹窗: (140,45) 30x110, 变化时滑入显示, 2秒无变化滑出隐藏 */
#define VOL_POP_X       140
#define VOL_POP_Y       45
#define VOL_POP_W       30
#define VOL_POP_H       110
#define VOL_POP_BAR_H   90
#define VOL_POP_HIDE_MS 2000
#define VOL_POP_ANIM_IN_MS   150   /* 滑入: overshoot 过冲(q弹), 从屏外冲到目标位再回落 */
#define VOL_POP_ANIM_OUT_MS  100   /* 滑出: 线性匀速 */

/* 用户命令发送: 与上一条间隔 < 500ms 则丢弃 */
#define CMD_MIN_INTERVAL_US  500000

/* ── 亮度抽屉: 左侧常驻容器, 收拢时只露灰条 (灰条贴容器右缘随容器移动) ── */
#define BRI_PANEL_W     30              /* 面板宽度 (窄一点: 展开动画覆盖面积小, 减少重绘) */
#define BRI_PANEL_H     150             /* 面板高度 */
#define BRI_PANEL_Y     45              /* 面板(容器内局部) y */
#define BRI_SLIDER_X    ((BRI_PANEL_W - 14) / 2)  /* 滑块(面板内局部) x (居中) */
#define BRI_SLIDER_Y    14              /* 滑块(面板内局部) y (留出上缘, 防滑块头超出面板) */
#define BRI_SLIDER_H    100             /* 滑块高 (缩短, 防顶部超出面板/底部压到数值) */
#define BRI_BAR_W       5               /* 灰条宽度 (贴面板右缘) */
#define BRI_BAR_X       (BRI_PANEL_W)   /* 灰条(容器内局部) x */
#define BRI_BAR_Y       50              /* 灰条(容器内局部) y */
#define BRI_BAR_H       80              /* 灰条高度 */
#define BRI_BAR_R       2               /* 灰条圆角 */
#define BRI_BTN_X       (BRI_PANEL_W)   /* 大触发按钮(容器内局部) x */
#define BRI_BTN_Y       46              /* 大触发按钮(容器内局部) y (避开顶部菜单按钮) */
#define BRI_BTN_W       20              /* 大触发按钮宽 (比灰条宽, 好点) */
#define BRI_BTN_H       100             /* 大触发按钮高 */
#define BRI_BTN_TUNE_CLR lv_color_hex(0xFF0000)  /* 触发按钮底色 (随即被设为全透明, 仅调试观察区域用) */
#define BRI_DRAW_W      (BRI_PANEL_W + BRI_BTN_W)  /* 容器总宽 (必须包住整个大按钮, 否则超出的部分会被容器裁切) */
#define BRI_DRAW_H      200             /* 容器总高 */
#define BRI_ANIM_IN_MS  150             /* 弹出: overshoot 过冲 */
#define BRI_ANIM_OUT_MS 300             /* 收回: ease_in (先慢后快) */

/* ── 电池图标: 顶部居中, 位于 n/n 标签上方 ── */
#define BAT_BODY_W      18
#define BAT_BODY_H      9
#define BAT_BODY_X      77
#define BAT_BODY_Y      7
#define BAT_NUB_W       2
#define BAT_NUB_H       4
#define BAT_PAD         2               /* 填充条距壳内边距 */
#define BAT_FILL_MAX_W  (BAT_BODY_W - 2 * BAT_PAD)
#define VBAT_PCT_MIN    3.3f
#define VBAT_PCT_MAX    4.2f

static lv_obj_t *s_bri_draw    = NULL;  /* 抽屉容器 (常驻) */
static lv_obj_t *s_bri_slider  = NULL;  /* 亮度滑块 */
static lv_obj_t *s_bri_val     = NULL;  /* 亮度数值标签 */
static lv_obj_t *s_bri_overlay = NULL;  /* 全屏透明按钮 (展开时存在) */
static bool      s_bri_expanded = false; /* 抽屉是否已展开 */

static void bri_open(void);
static void bri_close(void);

/* 播放器主界面控件句柄 */
static lv_obj_t *s_status_label;    /* 顶部状态标签 (如 "3/20") */
static lv_obj_t *s_bat_fill;        /* 电池图标填充条 */
static lv_obj_t *s_title_label;     /* 歌名标签 */
static lv_obj_t *s_artist_label;    /* 歌手标签 */
static lv_obj_t *s_progress_slider; /* 进度滑块 */
static lv_obj_t *s_time_current;    /* 当前时间标签 */
static lv_obj_t *s_time_total;      /* 总时长标签 */
static lv_obj_t *s_fmt_val;         /* 格式值 (MP3/FLAC/WAV) */
static lv_obj_t *s_sr_val;          /* 采样率值 */
static lv_obj_t *s_ch_val;          /* 声道数值 */
static lv_obj_t *s_bit_val;         /* 位深值 */
static lv_obj_t *s_album_art;       /* 封面容器 */
static lv_obj_t *s_album_img;       /* 封面图片控件 */
static lv_obj_t *s_album_icon;      /* 无封面时的默认图标 */
static lv_img_dsc_t s_cover_dsc;    /* 封面图像描述符 */
static lv_obj_t *s_play_icon;       /* 播放/暂停图标 */
static lv_obj_t *s_mode_icon;       /* 播放模式图标 */

/* 音量弹窗 */
static lv_obj_t *s_vol_cont = NULL;   /* 音量弹窗容器 */
static lv_obj_t *s_vol_bar  = NULL;   /* 音量条 */
static lv_obj_t *s_vol_val  = NULL;   /* 音量数值标签 */
static int32_t   s_vol_last_ui = -1;  /* 上次显示的音量 */
static int       s_vol_idle = 0;      /* 无变化计时 (ms) */
typedef enum {
    VOL_STATE_HIDDEN,   /* 隐藏 */
    VOL_STATE_SHOWING,  /* 滑入中 */
    VOL_STATE_HIDING,   /* 滑出中 */
} vol_state_t;
static vol_state_t s_vol_state = VOL_STATE_HIDDEN;

/* 播放列表: 当前文件夹联动 (仅读内存缓存 g_fs_cache, 不碰 SD) */
static char s_pl_group[FS_GROUP_MAX];   /* 当前播放列表所在分组 */
static int  s_pl_index = 0;             /* 当前播放项索引 */
static int  s_pl_count = 0;             /* 当前分组歌曲数 */

static play_mode_t s_play_mode = PLAY_MODE_SEQUENTIAL;  /* 播放模式 */
static int         s_loop_count = 1;           /* 随机/顺序: 每首重复次数 (1~9) */
static int         s_cur_plays = 1;            /* 当前曲已播放次数 */
static bool        s_auto_advancing = false;   /* 是否自动切歌中 (决定文件缺失时行为) */
static int64_t     s_last_user_cmd_us = 0;     /* 上次用户命令时刻 (节流) */
static bool        s_was_playing = false;      /* 影子播放状态 (蓝牙重连续播用) */

/* ── 删除文件流程 (异步: 等音频释放 SD → 请求系统任务删除 → 修位图 → 重扫) ── */
static lv_timer_t *s_del_timer = NULL;                       /* 驱动删除流程的两段式定时器 */
static char        s_del_group[FS_GROUP_MAX];               /* 待删文件的 group (先拷贝防失效) */
static char        s_del_name[FS_NAME_MAX];                 /* 待删文件名 */
static int         s_del_idx   = -1;                        /* 待删文件在文件夹内的索引 (修位图用) */
static bool        s_del_sent  = false;   /* 是否已把请求发给系统任务 */
static int64_t     s_del_t0    = 0;                         /* 当前阶段计时起点 (us) */

#define DEL_POLL_MS          20        /* 轮询周期 */
#define DEL_AUDIO_TIMEOUT_US 500000    /* 段1: 等音频释放 SD 的上限 500ms */
#define DEL_DONE_TIMEOUT_US  3000000   /* 段2: 等系统任务删除的上限 3s */

static void del_wait_audio_cb(lv_timer_t *tmr);

/* ── 文件不存在提示弹窗 ── */
static lv_obj_t *s_dialog = NULL;
static void show_file_not_found(void);
static void player_info_reset(void);

/* ── 播放列表辅助 ── */
/* 统计指定分组内的文件数 */
static int player_count_files(const char *group)
{
    if (!g_fs_cache || !group) return 0;
    int count = 0;
    for (int i = 0; i < g_fs_cache->count; i++) {
        fs_entry_t *e = &g_fs_cache->entries[i];
        if (!e->is_dir && strcmp(e->group, group) == 0) count++;
    }
    return count;
}

/* 取分组内第 idx 个文件名 (按缓存顺序) */
static const char *player_file_name_at(const char *group, int idx)
{
    if (!g_fs_cache || !group) return NULL;
    int seen = 0;
    for (int i = 0; i < g_fs_cache->count; i++) {
        fs_entry_t *e = &g_fs_cache->entries[i];
        if (e->is_dir || strcmp(e->group, group) != 0) continue;
        if (seen == idx) return e->name;
        seen++;
    }
    return NULL;
}

/* 更新顶部状态标签: "当前/总数" */
static void player_update_label(void)
{
    char buf[32];
    if (s_pl_count > 0) {
        snprintf(buf, sizeof(buf), "%d/%d", s_pl_index + 1, s_pl_count);
    } else {
        snprintf(buf, sizeof(buf), "PLAYER");
    }
    lv_label_set_text(s_status_label, buf);
}

/* 当前播放信息: 供文件浏览器打开时定位正在播放的歌曲 */
const char *player_current_group(void)
{
    if (s_pl_count <= 0) return NULL;
    return s_pl_group;
}

const char *player_current_name(void)
{
    if (s_pl_count <= 0) return NULL;
    return player_file_name_at(s_pl_group, s_pl_index);
}

/* 用户命令发送: 与上一条间隔 < 500ms 则丢弃 */
static bool audio_user_send(const audio_cmd_t *cmd)
{
    int64_t now = esp_timer_get_time();
    if (s_last_user_cmd_us != 0 && (now - s_last_user_cmd_us) < CMD_MIN_INTERVAL_US) {
        printf("[LVGL] 命令间隔<500ms, 忽略\n");
        return false;
    }
    s_last_user_cmd_us = now;
    xQueueSend(g_ui_audio_cmd_queue, cmd, 0);
    return true;
}

/* 封面: 立即清空引用, 隐藏图片控件, 显示默认图标 */
static void cover_clear(void)
{
    if (!s_album_img) return;
    lv_img_set_src(s_album_img, NULL);
    lv_obj_add_flag(s_album_img, LV_OBJ_FLAG_HIDDEN);
    if (s_album_icon) lv_obj_clear_flag(s_album_icon, LV_OBJ_FLAG_HIDDEN);
}

/* 封面: 让 img 控件指向解码任务给的 PSRAM 缓冲 (LVGL 不拷贝内存) */
static void cover_show(void *buf)
{
    if (!s_album_img) return;
    if (!buf) {
        /* 新歌无内嵌封面: 回退默认图标 */
        cover_clear();
        return;
    }
    s_cover_dsc.data = buf;
    lv_img_set_src(s_album_img, &s_cover_dsc);
    lv_obj_clear_flag(s_album_img, LV_OBJ_FLAG_HIDDEN);
    if (s_album_icon) lv_obj_add_flag(s_album_icon, LV_OBJ_FLAG_HIDDEN);
}

/* 显示解码完成的封面 (供 audio/cover 任务投递的指针调用) */
void player_show_cover(void *buf)
{
    cover_show(buf);
}

/* 封面容器长按: 有歌曲时弹出 喜欢/删除 窗口 */
static void album_art_long_press(void)
{
    printf("[LVGL] album art long press\n");
    if (!atomic_load_bool(&g_song_info_valid)) return;   /* 无歌曲: 忽略 */
    like_menu_open();
}

/* LVGL 事件回调: 封面长按 */
static void album_art_long_press_cb(lv_event_t *e)
{
    (void)e;
    album_art_long_press();
}

/* 按索引播放 (索引自动取模回绕), 解析路径并投递 AUDIO_CMD_PLAY */
static void player_play_index(int idx)
{
    if (s_pl_count <= 0) return;

    idx = (idx % s_pl_count + s_pl_count) % s_pl_count;

    const char *name = player_file_name_at(s_pl_group, idx);
    if (!name) return;

    char path[512];
    fs_build_real_path(s_pl_group, name, path, sizeof(path));

    audio_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = AUDIO_CMD_PLAY;
    size_t plen = strnlen(path, sizeof(cmd.path) - 1);
    memcpy(cmd.path, path, plen);
    cmd.path[plen] = '\0';

    if (!audio_user_send(&cmd)) return;

    s_pl_index = idx;
    s_cur_plays = 1;                 /* 新曲: 已播次数归 1 */
    s_auto_advancing = false;
    s_was_playing = true;
    printf("[LVGL] PLAY: %s\n", path);

    /* 随机模式: 切到该曲即标记为已播放 (即使只切不播) */
    if (s_play_mode == PLAY_MODE_RANDOM) played_bits_mark(s_pl_group, idx);

    player_update_label();

    /* 记录播放路径到 flash, 供下次插入 SD 卡时恢复 */
    last_song_save(path);
    /* 两向同步: 浏览器打开时刷新绿色高亮 */
    fs_browser_refresh();
}

/* 自动切歌(歌曲播完 / 跳过未找到): 直接发送, 不节流 */
void player_advance(void)
{
    if (s_pl_count <= 0) return;

    int next;
    switch (s_play_mode) {
    case PLAY_MODE_SINGLE:
        next = s_pl_index;
        break;
    case PLAY_MODE_RANDOM:
        if (s_pl_count <= 1) {
            next = 0;
        } else {
            /* 只在未播放 (位为 0) 的曲目中随机 */
            int p = played_bits_pick(s_pl_group);
            next = (p >= 0) ? p : 0;
        }
        break;
    case PLAY_MODE_SEQUENTIAL:
    default:
        next = (s_pl_index + 1) % s_pl_count;
        break;
    }

    const char *name = player_file_name_at(s_pl_group, next);
    if (!name) return;

    char path[512];
    fs_build_real_path(s_pl_group, name, path, sizeof(path));

    audio_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = AUDIO_CMD_PLAY;
    size_t plen = strnlen(path, sizeof(cmd.path) - 1);
    memcpy(cmd.path, path, plen);
    cmd.path[plen] = '\0';
    xQueueSend(g_ui_audio_cmd_queue, &cmd, 0);

    s_pl_index = next;
    s_cur_plays = 1;                 /* 新曲: 已播次数归 1 */
    s_auto_advancing = true;
    s_was_playing = true;
    printf("[LVGL] AUTO PLAY: %s (mode %d)\n", path, s_play_mode);

    /* 随机模式: 切到该曲即标记为已播放 */
    if (s_play_mode == PLAY_MODE_RANDOM) played_bits_mark(s_pl_group, next);

    player_update_label();

    /* 自动切歌同样记录路径并刷新浏览器高亮 */
    last_song_save(path);
    fs_browser_refresh();
}

/* 重播当前曲 (随机/顺序的重复播放用): 不动索引, 不重置已播次数 */
static void player_replay(void)
{
    const char *name = player_file_name_at(s_pl_group, s_pl_index);
    if (!name) { player_advance(); return; }

    char path[512];
    fs_build_real_path(s_pl_group, name, path, sizeof(path));

    audio_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = AUDIO_CMD_PLAY;
    size_t plen = strnlen(path, sizeof(cmd.path) - 1);
    memcpy(cmd.path, path, plen);
    cmd.path[plen] = '\0';
    xQueueSend(g_ui_audio_cmd_queue, &cmd, 0);

    s_auto_advancing = true;
    s_was_playing = true;
    printf("[LVGL] REPLAY %d/%d: %s\n", s_cur_plays, s_loop_count, path);
}

/* 当前曲是否"喜欢" (键与文件浏览器一致: 文件名去扩展名 → xxHash32) */
static bool current_song_is_liked(void)
{
    const char *name = player_current_name();
    if (!name) return false;
    char key[160];
    song_hash_name_key(name, key, sizeof(key));
    return likes_contains(song_hash32(key, strlen(key)));
}

/* 一首播放完毕: 喜欢歌未播满循环次数则重播, 否则前进到下一首 */
void player_on_song_finished(void)
{
    s_auto_advancing = true;

    /* 随机/顺序: 仅"喜欢"的歌未播满 s_loop_count 次才重播 (单曲循环不受此影响) */
    if ((s_play_mode == PLAY_MODE_RANDOM || s_play_mode == PLAY_MODE_SEQUENTIAL)
        && s_cur_plays < s_loop_count
        && current_song_is_liked()) {
        s_cur_plays++;
        player_replay();
        return;
    }

    player_advance();
}

/* 文件不存在: 自动前进中则跳过, 否则复位信息并弹"重新扫描"对话框 */
void player_on_file_not_found(void)
{
    if (s_auto_advancing) {
        player_advance();
    } else {
        cover_clear();
        atomic_store_bool(&g_song_info_valid, false);
        player_info_reset();
        show_file_not_found();
    }
}

/* 定时器回调: 两段式. 段1 等音频释放 SD; 段2 等系统任务删除完成, 再修位图并重扫 */
static void del_wait_audio_cb(lv_timer_t *tmr)
{
    if (!s_del_sent) {
        /* 段1: 等音频真正关闭解码器 (跨核并发, 必须等到它不再读 SD) */
        bool open    = atomic_load_bool(&g_audio_decoder_open);
        bool timeout = (esp_timer_get_time() - s_del_t0) > DEL_AUDIO_TIMEOUT_US;
        if (open && !timeout) return;

        /* 交给系统任务删除 (只置参数+标志, 立即返回, 不阻塞 UI) */
        if (!sd_request_delete_file(s_del_group, s_del_name, s_del_idx)) {
            lv_timer_del(tmr);
            s_del_timer = NULL;
            g_sd_manual_rescan = true;   /* 发不出去: 放弃并重扫恢复 UI */
            return;
        }
        s_del_sent = true;
        s_del_t0   = esp_timer_get_time();   /* 段2 计时起点 */
        return;
    }

    /* 段2: 等系统任务回报结果 (1=成功, -1=失败) */
    int st = g_sd_delete_status;
    if (st == 0) {
        if ((esp_timer_get_time() - s_del_t0) > DEL_DONE_TIMEOUT_US) st = -1;   /* 超时按失败 */
        else return;
    }
    g_sd_delete_status = 0;   /* 清零 */

    /* 成功才修位图 (纯 NVS, RAM 位图已被冻结释放); 失败不动位图 */
    if (st == 1) played_bits_remove_at(s_del_group, s_del_idx);

    /* 恢复 UI: 拔卡/插卡 + 全量重扫 (失败也重扫, 否则冻结状态卡住) */
    g_sd_manual_rescan = true;

    lv_timer_del(tmr);
    s_del_timer = NULL;
    s_del_sent  = false;
}

/* 请求删除当前文件: 冻结 UI → 停音频 → (定时器)请求系统任务删除 → 修位图 → 重扫 */
bool player_request_delete_current_file(void)
{
    if (s_del_timer || s_pl_count <= 0) return false;
    const char *name = player_current_name();
    if (!name) return false;

    /* 先拷贝: 冻结/重扫后 g_fs_cache 指针会失效 */
    snprintf(s_del_group, sizeof(s_del_group), "%s", s_pl_group);
    snprintf(s_del_name,  sizeof(s_del_name),  "%s", name);
    s_del_idx  = s_pl_index;
    s_del_sent = false;

    /* 1. 冻结 UI: 走拔卡路径(不卸载), 50ms 内清理: 停音频/复位播放器/清浏览器/置空缓存 */
    atomic_store_bool(&g_sd_ready, false);

    /* 2. 立即停音频 (清理分支也会发, 这里更及时) */
    audio_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = AUDIO_CMD_STOP;
    xQueueSend(g_ui_audio_cmd_queue, &cmd, 0);

    /* 3. 建定时器: 等音频停稳 → 请求系统任务删除 → 等结果 → 修位图 → 重扫 */
    s_del_t0    = esp_timer_get_time();
    s_del_timer = lv_timer_create(del_wait_audio_cb, DEL_POLL_MS, NULL);
    lv_timer_ready(s_del_timer);
    printf("[LVGL] 请求删除: %s\n", name);
    return true;
}

bool player_was_playing(void)
{
    return s_was_playing;
}

/* 设置"播放中"影子标志 (供蓝牙重连后续播判断) */
void player_set_was_playing(bool v)
{
    s_was_playing = v;
}

int player_get_loop_count(void)
{
    return s_loop_count;
}

/* 设置循环次数 (钳制到 1~9) 并持久化到 NVS */
void player_set_loop_count(int n)
{
    if (n < LOOP_COUNT_MIN) n = LOOP_COUNT_MIN;
    if (n > LOOP_COUNT_MAX) n = LOOP_COUNT_MAX;
    s_loop_count = n;
    settings_loop_count_save(n);
}

/* 按当前播放模式刷新模式按钮图标 */
static void mode_update_icon(void)
{
    const char *sym = LV_SYMBOL_LEFT;
    if (s_play_mode == PLAY_MODE_SINGLE) {
        sym = LV_SYMBOL_LOOP;
    } else if (s_play_mode == PLAY_MODE_RANDOM) {
        sym = LV_SYMBOL_SHUFFLE;
    }
    if (s_mode_icon) {
        lv_label_set_text(s_mode_icon, sym);
    }
}

static bool s_mode_long_pressed = false;   /* 长按已处理: 忽略松手后 LVGL 仍发的 CLICKED */

/* 模式按钮按下: 复位长按标志 */
static void mode_pressed_cb(lv_event_t *e)
{
    (void)e;
    s_mode_long_pressed = false;   /* 每次按下复位, 保证标志只对本次长按有效 */
}

/* 模式按钮长按: 打开循环次数设置弹窗, 并标记长按已处理 */
static void mode_long_press_cb(lv_event_t *e)
{
    (void)e;
    s_mode_long_pressed = true;
    mode_menu_open();
}

/* 模式按钮单击: 长按后的 CLICKED 吞掉, 否则轮换播放模式并持久化 */
static void mode_btn_click_cb(lv_event_t *e)
{
    if (s_mode_long_pressed) {   /* 长按后松手仍会发 CLICKED, 吞掉这一次 */
        s_mode_long_pressed = false;
        return;
    }

    s_play_mode = (play_mode_t)((s_play_mode + 1) % 3);
    mode_update_icon();
    printf("[LVGL] 播放模式: %d\n", s_play_mode);

    settings_mode_save(s_play_mode);
}

/* 播放指定分组下的文件: 建立播放列表并定位到该文件 (供文件浏览器/恢复上次歌曲调用) */
void player_play_file(const char *group, const char *name)
{
    if (!group || !name) return;

    strncpy(s_pl_group, group, sizeof(s_pl_group) - 1);
    s_pl_group[sizeof(s_pl_group) - 1] = '\0';
    s_pl_count = player_count_files(s_pl_group);

    int found = 0;
    for (int i = 0; i < s_pl_count; i++) {
        const char *n = player_file_name_at(s_pl_group, i);
        if (n && strcmp(n, name) == 0) {
            found = i;
            break;
        }
    }
    player_play_index(found);
}

/* 下一首 */
void player_next(void)
{
    if (s_pl_count <= 0) return;
    player_play_index(s_pl_index + 1);
}

/* 上一首 */
void player_prev(void)
{
    if (s_pl_count <= 0) return;
    player_play_index(s_pl_index - 1);
}

/* LVGL 事件回调: 上一首 / 下一首 */
static void player_prev_click_cb(lv_event_t *e) { player_prev(); }
static void player_next_click_cb(lv_event_t *e) { player_next(); }

/* 播放/暂停切换: 播放→暂停, 有歌续播, 无歌播当前项 (播放键 + 蓝牙耳机共用) */
void player_toggle_play(void)
{
    if (atomic_load_bool(&g_pcm_active)) {
        audio_cmd_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = AUDIO_CMD_PAUSE;
        if (audio_user_send(&cmd)) {
            printf("[LVGL] PAUSE\n");
        }
    } else if (atomic_load_bool(&g_song_info_valid) && s_pl_count > 0) {
        /* 暂停中且有已加载歌曲 → 纯续播: 不清封面, 绕过 500ms 节流 */
        const char *name = player_file_name_at(s_pl_group, s_pl_index);
        if (name) {
            char path[512];
            fs_build_real_path(s_pl_group, name, path, sizeof(path));
            audio_cmd_t cmd;
            memset(&cmd, 0, sizeof(cmd));
            cmd.type = AUDIO_CMD_PLAY;
            size_t plen = strnlen(path, sizeof(cmd.path) - 1);
            memcpy(cmd.path, path, plen);
            cmd.path[plen] = '\0';
            xQueueSend(g_ui_audio_cmd_queue, &cmd, 0);
            s_was_playing = true;
            printf("[LVGL] RESUME: %s\n", path);
        }
    } else {
        player_play_index(s_pl_index);
    }
}

/* LVGL 事件回调: 播放/暂停 */
static void play_btn_click_cb(lv_event_t *e)
{
    player_toggle_play();
}

/* 确认: 关弹窗 + 请求重新扫描 (走 sys_monitor 拔卡→插卡现有流程) */
static void dialog_rescan_cb(lv_event_t *e)
{
    if (s_dialog) {
        lv_obj_del(s_dialog);
        s_dialog = NULL;
    }
    g_sd_manual_rescan = true;
    printf("[LVGL] 请求重新扫描\n");
}

/* 弹出"文件不存在"对话框 (含重新扫描按钮) */
static void show_file_not_found(void)
{
    if (s_dialog) lv_obj_del(s_dialog);

    s_dialog = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_dialog, 150, 90);
    lv_obj_align(s_dialog, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_dialog, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_dialog, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_dialog, 8, 0);
    lv_obj_set_style_border_width(s_dialog, 0, 0);
    lv_obj_set_style_shadow_width(s_dialog, 24, 0);
    lv_obj_set_style_shadow_color(s_dialog, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(s_dialog, LV_OPA_60, 0);
    lv_obj_clear_flag(s_dialog, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(s_dialog);
    lv_label_set_text(lbl, "该文件不存在\n需重新扫描");
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_global_16, 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, -10);

    lv_obj_t *btn = lv_btn_create(s_dialog);
    lv_obj_set_size(btn, 64, 30);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, 7);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_bg_color(btn, COLOR_ACCENT, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, dialog_rescan_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, "确认");
    lv_obj_set_style_text_font(btn_lbl, &lv_font_global_16, 0);
    lv_obj_set_style_text_color(btn_lbl, lv_color_black(), 0);
    lv_obj_center(btn_lbl);
}

/* 进度滑块: 松手时发送跳转命令 (拖动中不发) */
static void progress_slider_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) return;
    if (!s_progress_slider) return;
    if (!atomic_load_bool(&g_song_info_valid)) return;

    audio_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = AUDIO_CMD_SEEK;
    cmd.param = (uint32_t)lv_slider_get_value(s_progress_slider);
    xQueueSend(g_ui_audio_cmd_queue, &cmd, 0);
    printf("[LVGL] SEEK: %" PRIu32 "%%\n", cmd.param / 10);
}

/* 无有效歌曲信息: 恢复占位文本 + 进度清零 */
static void player_info_reset(void)
{
    lv_label_set_text(s_title_label, "标题");
    lv_label_set_text(s_artist_label, "作者");
    lv_label_set_text(s_fmt_val, "");
    lv_label_set_text(s_sr_val, "");
    lv_label_set_text(s_ch_val, "");
    lv_label_set_text(s_bit_val, "");
    lv_label_set_text(s_time_current, "0:00");
    lv_label_set_text(s_time_total, "0:00");
    lv_slider_set_value(s_progress_slider, 0, LV_ANIM_OFF);
}

/* ── 歌曲信息轮询: 读 g_song_info 更新标签/进度 ── */
static void song_info_monitor_cb(lv_timer_t *timer)
{
    /* 电池图标: 读 g_vbat 换算百分比, 变化才更新填充条 */
    {
        static int s_last_pct = -1;
        float v = atomic_load_float(&g_vbat);
        int pct = (int)((v - VBAT_PCT_MIN) / (VBAT_PCT_MAX - VBAT_PCT_MIN) * 100.0f);
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;

        if (pct != s_last_pct) {
            s_last_pct = pct;
            int w = BAT_FILL_MAX_W * pct / 100;
            if (pct > 0 && w < 1) w = 1;
            lv_obj_set_width(s_bat_fill, w);
            lv_obj_set_style_bg_color(s_bat_fill,
                pct > 30 ? lv_color_hex(0x2E7D32) :
                pct >= 20 ? lv_color_hex(0xFFA000) : lv_color_hex(0xE53935), 0);
        }
    }

    /* 播放/暂停图标跟随实际播放状态 */
    if (s_play_icon) {
        lv_label_set_text(s_play_icon,
                          atomic_load_bool(&g_pcm_active) ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }

    song_info_t info;
    if (song_info_snapshot(&info)) {   /* 一次性取一致快照, 避免与解码任务并发撕裂 */
        lv_label_set_text(s_title_label, info.title[0] ? info.title : " ");
        lv_label_set_text(s_artist_label, info.artist[0] ? info.artist : " ");
        lv_label_set_text(s_fmt_val, info.format);

        char buf[16];
        /* 采样率: 整除1000 → "48k", 否则 "44.1k" */
        if (info.sample_rate % 1000 == 0) {
            snprintf(buf, sizeof(buf), "%" PRIu32 "k", info.sample_rate / 1000);
        } else {
            snprintf(buf, sizeof(buf), "%.1fk", (double)info.sample_rate / 1000.0);
        }
        lv_label_set_text(s_sr_val, buf);

        /* 声道 */
        lv_label_set_text(s_ch_val,
                          info.channels == 2 ? "2ch" :
                          (info.channels == 1 ? "1ch" : " "));

        /* 右下: MP3 → 码率, FLAC/WAV → 位深 */
        if (strcmp(info.format, "MP3") == 0) {
            snprintf(buf, sizeof(buf), "%" PRIu32 "k", info.bitrate_kbps);
        } else {
            snprintf(buf, sizeof(buf), "%ubit", info.bits_per_sample);
        }
        lv_label_set_text(s_bit_val, buf);

        uint32_t cur = info.elapsed_sec;
        uint32_t tot = info.duration_sec;
        snprintf(buf, sizeof(buf), "%" PRIu32 ":%02" PRIu32, cur / 60, cur % 60);
        lv_label_set_text(s_time_current, buf);
        snprintf(buf, sizeof(buf), "%" PRIu32 ":%02" PRIu32, tot / 60, tot % 60);
        lv_label_set_text(s_time_total, buf);

        if (tot > 0) {
            int val = (int)((uint64_t)cur * 1000 / tot);
            if (val > 1000) val = 1000;
            if (!lv_slider_is_dragged(s_progress_slider)) {
                lv_slider_set_value(s_progress_slider, val, LV_ANIM_OFF);
            }
        }
    } else {
        player_info_reset();
    }
}

/* SD 卡状态监视 (轮询定时器): 插卡加载/拔卡清理 */
static void fs_sd_monitor_cb(lv_timer_t *timer)
{
    static bool last_ready = false;
    bool sd_ready = atomic_load_bool(&g_sd_ready);

    /* 回收 sys_monitor 移交的旧缓存 (本任务即所有 g_fs_cache 读者, 此刻必无并发遍历) */
    fs_cache_reap();

    if (!sd_ready && last_ready) {   /* 刚拔出 */
        /* 音频立即停止, 干净 close_decoder */
        audio_cmd_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = AUDIO_CMD_STOP;
        xQueueSend(g_ui_audio_cmd_queue, &cmd, 0);
        s_was_playing = false;

        /* 播放器状态复位: 残留的 SONG_FINISHED→advance 全部 no-op */
        s_pl_count = 0;
        s_pl_index = 0;
        player_update_label();

        /* 置空缓存指针: 浏览器/播放器立即 inert, 重开文件管理器也是空列表.
         * deinit 里还会再置一次并 free(s_fs_cache_owned), 重复置空无害. */
        g_fs_cache = NULL;

        cover_clear();

        fs_browser_on_sd_remove();   /* 通知浏览器清空 */
        played_bits_on_sd_remove();  /* 释放随机去重位图 */
    }

    if (sd_ready && !last_ready) {   /* 刚插入 */
        fs_browser_on_sd_ready();
        played_bits_on_sd_ready();   /* 全量校验并载入各文件夹随机去重位图 */

        /* 恢复上次歌曲: 读 flash 路径 → 在缓存中查找 → 找到则加载到解码器但不自动播放,
         * 紧随其后发一个暂停; 主UI跳到该曲, 浏览器同步高亮 */
        char saved[512];
        if (last_song_load(saved, sizeof(saved))) {
            char group[FS_GROUP_MAX];
            char name[FS_NAME_MAX];
            if (fs_cache_find_by_path(saved, group, sizeof(group), name, sizeof(name))) {
                player_play_file(group, name);

                /* 只加载不播放: 直接发暂停 (不用 audio_user_send, 避免被节流丢弃) */
                audio_cmd_t cmd;
                memset(&cmd, 0, sizeof(cmd));
                cmd.type = AUDIO_CMD_PAUSE;
                xQueueSend(g_ui_audio_cmd_queue, &cmd, 0);

                fs_browser_jump();   /* 浏览器定位到该曲 */
            }
        }
    }

    last_ready = sd_ready;
}

/* 创建音量弹窗(初始隐藏), 值变化时由定时器显示更新 */
static void vol_popup_create(void)
{
    s_vol_cont = lv_obj_create(lv_scr_act());
    lv_obj_set_pos(s_vol_cont, TFT_HOR_RES, VOL_POP_Y);  /* 初始在屏外, 由动画滑入 */
    lv_obj_set_size(s_vol_cont, VOL_POP_W, VOL_POP_H);
    lv_obj_set_style_bg_color(s_vol_cont, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_vol_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_vol_cont, 6, 0);
    lv_obj_set_style_border_width(s_vol_cont, 0, 0);
    lv_obj_set_style_shadow_width(s_vol_cont, 0, 0);
    lv_obj_set_style_pad_all(s_vol_cont, 0, 0);
    lv_obj_clear_flag(s_vol_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_vol_cont, LV_SCROLLBAR_MODE_OFF);
    /* 兜底: 滚动条透明(内容溢出时也不可见), 邪修方案 */
    lv_obj_set_style_bg_opa(s_vol_cont, LV_OPA_TRANSP, LV_PART_SCROLLBAR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_vol_cont, LV_OPA_TRANSP, LV_PART_SCROLLBAR | LV_STATE_SCROLLED);

    s_vol_bar = lv_bar_create(s_vol_cont);
    lv_obj_set_pos(s_vol_bar, (VOL_POP_W - 22) / 2, 4);
    lv_obj_set_size(s_vol_bar, 22, VOL_POP_BAR_H);
    lv_obj_set_style_bg_color(s_vol_bar, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_style_bg_opa(s_vol_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_vol_bar, 3, 0);
    lv_obj_set_style_radius(s_vol_bar, 3, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_vol_bar, COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_vol_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_bar_set_range(s_vol_bar, 0, VOLUME_MAX);
    lv_bar_set_value(s_vol_bar, volume_get(), LV_ANIM_OFF);

    s_vol_val = lv_label_create(s_vol_cont);
    lv_obj_set_pos(s_vol_val, 0, 4 + VOL_POP_BAR_H + 2);
    lv_obj_set_size(s_vol_val, VOL_POP_W, 18);
    lv_obj_set_style_text_align(s_vol_val, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_vol_val, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_vol_val, lv_color_black(), 0);
    lv_label_set_text_fmt(s_vol_val, "%" PRId32, volume_get());

    lv_obj_add_flag(s_vol_cont, LV_OBJ_FLAG_HIDDEN);
    s_vol_last_ui = volume_get();
}

/* 动画 exec 适配器: 直接把动画值设为弹窗 x 坐标 */
static void vol_popup_set_x(void *obj, int32_t x)
{
    lv_obj_set_x((lv_obj_t *)obj, (lv_coord_t)x);
}

/* 音量弹窗: 从屏外滑入, overshoot 过冲(q弹)后回落目标位 */
static void vol_popup_slide_in(void)
{
    lv_anim_del(s_vol_cont, NULL);   /* 中断可能进行中的滑出 */
    lv_obj_move_foreground(s_vol_cont);
    lv_obj_clear_flag(s_vol_cont, LV_OBJ_FLAG_HIDDEN);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_vol_cont);
    lv_anim_set_exec_cb(&a, vol_popup_set_x);
    lv_anim_set_values(&a, TFT_HOR_RES, VOL_POP_X);
    lv_anim_set_time(&a, VOL_POP_ANIM_IN_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
    lv_anim_start(&a);
    s_vol_state = VOL_STATE_SHOWING;
}

/* 滑出动画完成: 移出屏外后隐藏 */
static void vol_popup_slide_out_end(lv_anim_t *a)
{
    lv_obj_add_flag(s_vol_cont, LV_OBJ_FLAG_HIDDEN);
    s_vol_state = VOL_STATE_HIDDEN;
}

/* 音量弹窗: 匀速滑出屏外 */
static void vol_popup_slide_out(void)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_vol_cont);
    lv_anim_set_exec_cb(&a, vol_popup_set_x);
    lv_anim_set_values(&a, lv_obj_get_x(s_vol_cont), TFT_HOR_RES);
    lv_anim_set_time(&a, VOL_POP_ANIM_OUT_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_set_ready_cb(&a, vol_popup_slide_out_end);
    lv_anim_start(&a);
    s_vol_state = VOL_STATE_HIDING;
}

/* 音量轮询 (5Hz): 值变化则滑入弹窗并写入 NVS, 2秒无变化滑出隐藏 */
static void volume_monitor_cb(lv_timer_t *timer)
{
    int32_t v = volume_get();

    if (v != s_vol_last_ui) {
        s_vol_last_ui = v;
        s_vol_idle = 0;
        lv_bar_set_value(s_vol_bar, v, LV_ANIM_OFF);
        lv_label_set_text_fmt(s_vol_val, "%" PRId32, v);
        if (s_vol_state == VOL_STATE_HIDDEN) {
            vol_popup_slide_in();
        } else if (s_vol_state == VOL_STATE_HIDING) {
            vol_popup_slide_in();   /* 滑出中被调回, 重新滑入 */
        }
        /* SHOWING 状态: 已显示, 仅刷新数值 */
    } else {
        s_vol_idle++;
        if (s_vol_state == VOL_STATE_SHOWING &&
            s_vol_idle >= VOL_POP_HIDE_MS / 200) {
            vol_popup_slide_out();
        }
    }

    volume_save_to_nvs();
}

/* 音量键状态: 上升沿单步 + 长按(>600ms)后每 150ms 重复 */
typedef struct {
    bool    prev;       /* 上次采样电平 (上升沿检测) */
    bool    repeating;  /* 是否已进入长按重复模式 */
    int64_t next_us;    /* 下次允许步进的时刻 (us) */
} vol_key_state_t;
static vol_key_state_t s_vol_up, s_vol_down;   /* 音量+ / 音量- 各自的状态机 */

/* 单个音量键状态机: pin=引脚, dir=+1/-1, st=该键状态 */
static void vol_key_poll_one(int pin, int dir, vol_key_state_t *st)
{
    bool    level = (gpio_get_level(pin) == 1);   /* 高=按下 (外部下拉) */
    int64_t now   = esp_timer_get_time();

    if (level) {
        if (!st->prev) {                          /* 上升沿: 立即 ±4 一次 */
            volume_inc(dir * VOLUME_STEP);
            st->repeating = false;
            st->next_us   = now + VOL_LONGPRESS_MS * 1000LL;
        } else if (!st->repeating) {
            if (now >= st->next_us) {             /* 按住超过 600ms: 进入长按, 补一步 */
                st->repeating = true;
                volume_inc(dir * VOLUME_STEP);
                st->next_us = now + VOL_REPEAT_MS * 1000LL;
            }
        } else if (now >= st->next_us) {          /* 长按中: 每 150ms ±4 */
            volume_inc(dir * VOLUME_STEP);
            st->next_us = now + VOL_REPEAT_MS * 1000LL;
        }
    } else {
        st->repeating = false;                    /* 松开: 退出重复 */
    }
    st->prev = level;
}

/* 按键轮询 (10ms): 息屏/唤醒键(上升沿) + 音量键增减.
 * 之前放在 sys_monitor(10Hz) 会漏掉 <100ms 的短按, 移入 LVGL 用 10ms 轮询.
 * 息屏键采样放最前(不受音量节流影响), 由 power_mgr 做上升沿检测 */
static void btn_key_poll_cb(lv_timer_t *timer)
{
    power_mgr_poll_key(gpio_get_level(PIN_PWR_KEY) == 1);

    vol_key_poll_one(PIN_VOL_UP,   +1, &s_vol_up);
    vol_key_poll_one(PIN_VOL_DOWN, -1, &s_vol_down);
}

/* 初始化音量按键: 配输入引脚 + 创建 10ms 轮询定时器 */
static void vol_key_init(void)
{
    gpio_set_direction(PIN_VOL_UP, GPIO_MODE_INPUT);
    gpio_set_direction(PIN_VOL_DOWN, GPIO_MODE_INPUT);
    lv_timer_create(btn_key_poll_cb, VOL_KEY_POLL_MS, NULL);
}

/* ── 亮度抽屉 ── */

/* 滑块事件: 拖动实时改亮度 (写 LEDC), 松手存 NVS */
static void bri_slider_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        uint8_t v = (uint8_t)lv_slider_get_value(sl);
        lcd_set_brightness(v);           /* 立即生效 */
        brightness_set(v);               /* 更新内存值 */
        power_mgr_set_cur_bri(v);        /* 同步电源管理的淡入/淡出起点 */
        lv_label_set_text_fmt(s_bri_val, "%d", (int)v);
    } else if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        brightness_save_to_nvs();        /* 松手落盘 */
    }
}

/* 动画 exec 适配器: 把动画值设为抽屉容器 x 坐标 */
static void bri_draw_set_x(void *obj, int32_t x)
{
    lv_obj_set_x((lv_obj_t *)obj, (lv_coord_t)x);
}

/* 收起完成: 注销全屏透明按钮, 抽屉回到收拢态只露灰条 */
static void bri_close_end(lv_anim_t *a)
{
    if (s_bri_overlay) {
        lv_obj_del(s_bri_overlay);
        s_bri_overlay = NULL;
    }
    brightness_save_to_nvs();
}

/* 收起: 抽屉滑回屏外 */
static void bri_close(void)
{
    if (!s_bri_expanded) return;
    s_bri_expanded = false;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_bri_draw);
    lv_anim_set_exec_cb(&a, bri_draw_set_x);
    lv_anim_set_values(&a, 0, -BRI_PANEL_W);
    lv_anim_set_time(&a, BRI_ANIM_OUT_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);   /* 回缩: 先慢后快 */
    lv_anim_set_ready_cb(&a, bri_close_end);
    lv_anim_start(&a);
}

/* 全屏透明按钮: 点容器外任意处 → 收起 */
static void bri_overlay_click_cb(lv_event_t *e)
{
    bri_close();
}

/* 弹出: 创建全屏透明按钮 → 亮度容器置顶 → 滑出 */
static void bri_open(void)
{
    if (s_bri_expanded) return;
    s_bri_expanded = true;

    if (!s_bri_overlay) {
        s_bri_overlay = lv_btn_create(lv_scr_act());
        lv_obj_set_pos(s_bri_overlay, 0, 0);
        lv_obj_set_size(s_bri_overlay, TFT_HOR_RES, TFT_VER_RES);
        lv_obj_set_style_bg_opa(s_bri_overlay, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_bri_overlay, 0, 0);
        lv_obj_set_style_shadow_width(s_bri_overlay, 0, 0);
        lv_obj_clear_flag(s_bri_overlay, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(s_bri_overlay, bri_overlay_click_cb, LV_EVENT_CLICKED, NULL);
    }

    /* 亮度容器置顶: 压过透明按钮, 保证面板可操作 */
    lv_obj_move_foreground(s_bri_draw);

    /* 刷新当前亮度到滑块/标签 */
    lv_slider_set_value(s_bri_slider, brightness_get(), LV_ANIM_OFF);
    lv_label_set_text_fmt(s_bri_val, "%d", (int)brightness_get());

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_bri_draw);
    lv_anim_set_exec_cb(&a, bri_draw_set_x);
    lv_anim_set_values(&a, -BRI_PANEL_W, 0);
    lv_anim_set_time(&a, BRI_ANIM_IN_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
    lv_anim_start(&a);
}

/* 大触发按钮: 收拢时点击弹出, 展开时点击收回 */
static void bri_btn_click_cb(lv_event_t *e)
{
    if (s_bri_expanded) {
        bri_close();
    } else {
        bri_open();
    }
}

/* 创建亮度抽屉 (常驻, 初始收拢: 容器 x=-BRI_PANEL_W, 只露灰条在屏幕左缘 x=0..5) */
static void bri_draw_create(void)
{
    s_bri_draw = lv_obj_create(lv_scr_act());
    lv_obj_set_pos(s_bri_draw, -BRI_PANEL_W, 0);
    lv_obj_set_size(s_bri_draw, BRI_DRAW_W, BRI_DRAW_H);
    lv_obj_set_style_bg_opa(s_bri_draw, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_bri_draw, 0, 0);
    lv_obj_set_style_radius(s_bri_draw, 0, 0);
    lv_obj_set_style_pad_all(s_bri_draw, 0, 0);
    lv_obj_clear_flag(s_bri_draw, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_bri_draw, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scrollbar_mode(s_bri_draw, LV_SCROLLBAR_MODE_OFF);

    /* 白色面板 (亮度容器): 点击内部不落到透明按钮 */
    lv_obj_t *panel = lv_obj_create(s_bri_draw);
    lv_obj_set_pos(panel, 0, BRI_PANEL_Y);
    lv_obj_set_size(panel, BRI_PANEL_W, BRI_PANEL_H);
    lv_obj_set_style_bg_color(panel, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 6, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_shadow_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    /* 竖向亮度滑块 (高>宽自动竖排), range 1~255 */
    s_bri_slider = lv_slider_create(panel);
    lv_obj_set_pos(s_bri_slider, BRI_SLIDER_X, BRI_SLIDER_Y);
    lv_obj_set_size(s_bri_slider, 14, BRI_SLIDER_H);
    lv_slider_set_range(s_bri_slider, BRIGHTNESS_MIN, BRIGHTNESS_MAX);
    lv_slider_set_value(s_bri_slider, brightness_get(), LV_ANIM_OFF);
    lv_obj_set_style_radius(s_bri_slider, 7, 0);
    lv_obj_set_style_radius(s_bri_slider, 7, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bri_slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_bg_color(s_bri_slider, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_style_bg_opa(s_bri_slider, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_bri_slider, COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_bri_slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_bri_slider, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s_bri_slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_border_color(s_bri_slider, COLOR_ACCENT, LV_PART_KNOB);
    lv_obj_set_style_border_width(s_bri_slider, 2, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_bri_slider, 0, LV_PART_KNOB);
    lv_obj_set_style_transform_width(s_bri_slider, 2, LV_PART_KNOB);
    lv_obj_set_style_transform_height(s_bri_slider, 2, LV_PART_KNOB);
    lv_obj_add_event_cb(s_bri_slider, bri_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_bri_slider, bri_slider_cb, LV_EVENT_RELEASED, NULL);

    /* 亮度数值标签 */
    s_bri_val = lv_label_create(panel);
    lv_obj_set_pos(s_bri_val, 0, BRI_PANEL_H - 18);
    lv_obj_set_size(s_bri_val, BRI_PANEL_W, 16);
    lv_obj_set_style_text_align(s_bri_val, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_bri_val, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_bri_val, lv_color_black(), 0);
    lv_label_set_text_fmt(s_bri_val, "%d", (int)brightness_get());

    /* 灰条 (纯提示: 告诉用户点哪里, 不接收点击; 深色低对比 + 圆角) */
    lv_obj_t *bar = lv_obj_create(s_bri_draw);
    lv_obj_set_pos(bar, BRI_BAR_X, BRI_BAR_Y);
    lv_obj_set_size(bar, BRI_BAR_W, BRI_BAR_H);
    lv_obj_set_style_radius(bar, BRI_BAR_R, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_shadow_width(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* 大触发按钮 (透明: 点击区域比灰条大, 收拢时屏幕 x≈0..20 / y=46..146 都能点到) */
    lv_obj_t *btn = lv_btn_create(s_bri_draw);
    lv_obj_set_pos(btn, BRI_BTN_X, BRI_BTN_Y);
    lv_obj_set_size(btn, BRI_BTN_W, BRI_BTN_H);
    lv_obj_set_style_radius(btn, 0, 0);
    lv_obj_set_style_bg_color(btn, BRI_BTN_TUNE_CLR, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn, bri_btn_click_cb, LV_EVENT_CLICKED, NULL);
}

/* ── 主界面构建 ── */
/* 初始化播放器界面: 恢复设置, 建控件, 建各定时器 (SD/歌曲信息/音量/按键) */
void ui_player_init(void)
{
    play_mode_t m;
    if (settings_mode_load(&m)) s_play_mode = m;
    s_loop_count = settings_loop_count_load();
    s_cur_plays  = 1;

    lv_obj_t *scr = lv_scr_act();

    /* ── 屏幕底色 ── */
    lv_obj_set_style_bg_color(scr, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    /* 关闭屏幕自身滚动条: 弹窗滑到屏外会撑大屏幕内容触发, 文件浏览器滚动条在各自 list 内部不受影响 */
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* ── TOP BAR ── */
    /* 菜单按钮 */
    lv_obj_t *btn_menu = make_icon_btn(scr, 5, 5, 40, 40, LV_SYMBOL_LIST);
    lv_obj_t *icon_menu = lv_obj_get_child(btn_menu, 0);
    lv_obj_set_style_text_font(icon_menu, &lv_font_montserrat_18, 0);
    lv_obj_add_event_cb(btn_menu, fs_menu_click_cb, LV_EVENT_CLICKED, NULL);

    /* 蓝牙按钮 */
    lv_obj_t *btn_ble = make_icon_btn(scr, 126, 5, 40, 40, LV_SYMBOL_BLUETOOTH);
    lv_obj_t *icon_ble = lv_obj_get_child(btn_ble, 0);
    lv_obj_set_style_text_font(icon_ble, &lv_font_montserrat_20, 0);
    lv_obj_add_event_cb(btn_ble, bt_menu_click_cb, LV_EVENT_CLICKED, NULL);

    /* 文字 */
    s_status_label = lv_label_create(scr);
    lv_obj_set_pos(s_status_label, 54, 20);
    lv_obj_set_size(s_status_label, 65, 16);
    lv_obj_set_style_text_align(s_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_status_label, COLOR_MUTED, 0);
    lv_label_set_text(s_status_label, "PLAYER");

    /* 电池图标 (n/n 上方) */
    lv_obj_t *bat_body = lv_obj_create(scr);
    lv_obj_set_pos(bat_body, BAT_BODY_X, BAT_BODY_Y);
    lv_obj_set_size(bat_body, BAT_BODY_W, BAT_BODY_H);
    lv_obj_set_style_bg_opa(bat_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(bat_body, 2, 0);
    lv_obj_set_style_border_color(bat_body, COLOR_MUTED, 0);
    lv_obj_set_style_border_width(bat_body, 1, 0);
    lv_obj_set_style_pad_all(bat_body, 0, 0);
    lv_obj_set_style_shadow_width(bat_body, 0, 0);
    lv_obj_clear_flag(bat_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(bat_body, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *bat_nub = lv_obj_create(scr);
    lv_obj_set_pos(bat_nub, BAT_BODY_X + BAT_BODY_W, BAT_BODY_Y + (BAT_BODY_H - BAT_NUB_H) / 2);
    lv_obj_set_size(bat_nub, BAT_NUB_W, BAT_NUB_H);
    lv_obj_set_style_bg_color(bat_nub, COLOR_MUTED, 0);
    lv_obj_set_style_bg_opa(bat_nub, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bat_nub, 1, 0);
    lv_obj_set_style_border_width(bat_nub, 0, 0);
    lv_obj_set_style_shadow_width(bat_nub, 0, 0);
    lv_obj_clear_flag(bat_nub, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(bat_nub, LV_SCROLLBAR_MODE_OFF);

    s_bat_fill = lv_obj_create(scr);
    lv_obj_set_pos(s_bat_fill, BAT_BODY_X + BAT_PAD, BAT_BODY_Y + BAT_PAD);
    lv_obj_set_size(s_bat_fill, BAT_FILL_MAX_W, BAT_BODY_H - 2 * BAT_PAD);
    lv_obj_set_style_bg_color(s_bat_fill, lv_color_hex(0x2E7D32), 0);
    lv_obj_set_style_bg_opa(s_bat_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_bat_fill, 1, 0);
    lv_obj_set_style_border_width(s_bat_fill, 0, 0);
    lv_obj_set_style_shadow_width(s_bat_fill, 0, 0);
    lv_obj_clear_flag(s_bat_fill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_bat_fill, LV_SCROLLBAR_MODE_OFF);

    /* 专辑封面区域 */
    s_album_art = lv_obj_create(scr);
    lv_obj_set_pos(s_album_art, 35, 40);
    lv_obj_set_size(s_album_art, 100, 100);
    lv_obj_set_style_radius(s_album_art, 20, 0);
    lv_obj_set_style_bg_color(s_album_art, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_opa(s_album_art, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_album_art, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(s_album_art, LV_OPA_10, 0);
    lv_obj_set_style_border_width(s_album_art, 1, 0);
    lv_obj_set_style_shadow_color(s_album_art, lv_color_black(), 0);
    lv_obj_set_style_shadow_width(s_album_art, 24, 0);
    lv_obj_set_style_shadow_opa(s_album_art, LV_OPA_60, 0);
    /* 子元素(封面图)按容器圆角裁剪 */
    lv_obj_set_style_clip_corner(s_album_art, true, 0);
    /* 封面铺满容器: 去内边距 + 禁止滚动 */
    lv_obj_set_style_pad_all(s_album_art, 0, 0);
    lv_obj_clear_flag(s_album_art, LV_OBJ_FLAG_SCROLLABLE);
    /* 长按封面容器: 触发一次 (默认阈值 400ms) */
    lv_obj_add_event_cb(s_album_art, album_art_long_press_cb, LV_EVENT_LONG_PRESSED, NULL);

    lv_obj_t *album_icon = lv_label_create(s_album_art);
    lv_label_set_text(album_icon, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_color(album_icon, COLOR_ACCENT, 0);
    lv_obj_set_style_text_opa(album_icon, LV_OPA_40, 0);
    lv_obj_set_style_text_font(album_icon, &lv_font_montserrat_20, 0);
    lv_obj_center(album_icon);
    s_album_icon = album_icon;

    /* 封面 img: 只指向解码任务写好的 PSRAM RGB565, LVGL 不拷贝 */
    s_cover_dsc.header.always_zero = 0;
    s_cover_dsc.header.w = 100;
    s_cover_dsc.header.h = 100;
    s_cover_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_cover_dsc.data_size = 100 * 100 * 2;
    s_cover_dsc.data = NULL;

    s_album_img = lv_img_create(s_album_art);
    lv_obj_set_pos(s_album_img, 0, 0);
    lv_obj_set_size(s_album_img, 100, 100);
    lv_img_set_src(s_album_img, NULL);
    lv_obj_add_flag(s_album_img, LV_OBJ_FLAG_HIDDEN);

    /* ── 歌曲信息区域 ── */
    s_title_label = lv_label_create(scr);
    lv_obj_set_pos(s_title_label, 8, 148);
    lv_obj_set_size(s_title_label, 156, 20);
    lv_obj_set_style_text_align(s_title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_title_label, &lv_font_global_16, 0);
    lv_obj_set_style_text_color(s_title_label, COLOR_FG, 0);
    lv_label_set_long_mode(s_title_label, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_title_label, "标题");

    s_artist_label = lv_label_create(scr);
    lv_obj_set_pos(s_artist_label, 8, 170);
    lv_obj_set_size(s_artist_label, 156, 16);
    lv_obj_set_style_text_align(s_artist_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_artist_label, &lv_font_global_16, 0);
    lv_obj_set_style_text_color(s_artist_label, COLOR_MUTED, 0);
    lv_label_set_long_mode(s_artist_label, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_artist_label, "作者");

    /* ── 播放进度滑块 ── */
    s_progress_slider = lv_slider_create(scr);
    lv_obj_set_pos(s_progress_slider, 14, 194);
    lv_obj_set_size(s_progress_slider, 144, 4);
    lv_obj_set_style_radius(s_progress_slider, 2, 0);
    lv_obj_set_style_radius(s_progress_slider, 2, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_progress_slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_bg_color(s_progress_slider, COLOR_BORDER, 0);
    lv_obj_set_style_bg_opa(s_progress_slider, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_progress_slider, COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_progress_slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_progress_slider, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s_progress_slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_border_color(s_progress_slider, COLOR_ACCENT, LV_PART_KNOB);
    lv_obj_set_style_border_width(s_progress_slider, 2, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_progress_slider, 0, LV_PART_KNOB);
    lv_obj_set_style_transform_width(s_progress_slider, 2, LV_PART_KNOB);
    lv_obj_set_style_transform_height(s_progress_slider, 2, LV_PART_KNOB);
    lv_slider_set_range(s_progress_slider, 0, 1000);
    lv_slider_set_value(s_progress_slider, 0, LV_ANIM_OFF);
    lv_obj_add_event_cb(s_progress_slider, progress_slider_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_progress_slider, progress_slider_cb, LV_EVENT_PRESS_LOST, NULL);

    s_time_current = lv_label_create(scr);
    lv_obj_set_pos(s_time_current, 14, 204);
    lv_obj_set_style_text_font(s_time_current, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_time_current, COLOR_ACCENT, 0);
    lv_label_set_text(s_time_current, "0:00");

    s_time_total = lv_label_create(scr);
    lv_obj_set_pos(s_time_total, 110, 204);
    lv_obj_set_size(s_time_total, 48, 12);
    lv_obj_set_style_text_align(s_time_total, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(s_time_total, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_time_total, COLOR_MUTED, 0);
    lv_label_set_text(s_time_total, "0:00");

    /* ── 播放控制按钮 ── */
    /* 播放 / 暂停 */
    lv_obj_t *play_btn = lv_btn_create(scr);
    lv_obj_set_pos(play_btn, 63, 212);
    lv_obj_set_size(play_btn, 46, 46);
    lv_obj_set_style_radius(play_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(play_btn, COLOR_ACCENT, 0);
    lv_obj_set_style_bg_opa(play_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(play_btn, 0, 0);
    lv_obj_set_style_shadow_color(play_btn, COLOR_ACCENT, 0);
    lv_obj_set_style_shadow_width(play_btn, 12, 0);
    lv_obj_set_style_shadow_opa(play_btn, LV_OPA_30, 0);

    s_play_icon = lv_label_create(play_btn);
    lv_label_set_text(s_play_icon, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_color(s_play_icon, lv_color_black(), 0);
    lv_obj_set_style_text_font(s_play_icon, &lv_font_montserrat_20, 0);
    lv_obj_center(s_play_icon);
    lv_obj_add_event_cb(play_btn, play_btn_click_cb, LV_EVENT_CLICKED, NULL);

    /* 上一首 (透明点击区放大到 60x64, 符号位置不变) */
    lv_obj_t *prev_btn = lv_btn_create(scr);
    lv_obj_set_pos(prev_btn, 0, 206);
    lv_obj_set_size(prev_btn, 60, 64);
    lv_obj_set_style_radius(prev_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(prev_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(prev_btn, LV_OPA_0, 0);
    lv_obj_set_style_border_width(prev_btn, 0, 0);
    lv_obj_set_style_shadow_width(prev_btn, 0, 0);

    lv_obj_t *prev_icon = lv_label_create(prev_btn);
    lv_label_set_text(prev_icon, LV_SYMBOL_PREV);
    lv_obj_set_style_text_color(prev_icon, COLOR_MUTED, 0);
    lv_obj_set_style_text_font(prev_icon, &lv_font_montserrat_20, 0);
    lv_obj_align(prev_icon, LV_ALIGN_CENTER, 13, 0);   /* 偏移补偿放大: 符号仍在 (43,238) */
    lv_obj_add_event_cb(prev_btn, player_prev_click_cb, LV_EVENT_CLICKED, NULL);

    /* 下一首 (透明点击区放大到 60x64, 符号位置不变) */
    lv_obj_t *next_btn = lv_btn_create(scr);
    lv_obj_set_pos(next_btn, 112, 206);
    lv_obj_set_size(next_btn, 60, 64);
    lv_obj_set_style_radius(next_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(next_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(next_btn, LV_OPA_0, 0);
    lv_obj_set_style_border_width(next_btn, 0, 0);
    lv_obj_set_style_shadow_width(next_btn, 0, 0);

    lv_obj_t *next_icon = lv_label_create(next_btn);
    lv_label_set_text(next_icon, LV_SYMBOL_NEXT);
    lv_obj_set_style_text_color(next_icon, COLOR_MUTED, 0);
    lv_obj_set_style_text_font(next_icon, &lv_font_montserrat_20, 0);
    lv_obj_align(next_icon, LV_ALIGN_CENTER, -13, 0);  /* 偏移补偿放大: 符号仍在 (129,238) */
    lv_obj_add_event_cb(next_btn, player_next_click_cb, LV_EVENT_CLICKED, NULL);

    /* ── 底部面板 ── */
    lv_obj_t *panel = lv_obj_create(scr);
    lv_obj_set_pos(panel, 4, 272);
    lv_obj_set_size(panel, 164, 46);
    lv_obj_set_style_radius(panel, 16, 0);
    lv_obj_set_style_bg_color(panel, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x222222), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);

    /* 模式按钮 */
    lv_obj_t *mode_btn = lv_btn_create(panel);
    lv_obj_set_pos(mode_btn, 5, 5);
    lv_obj_set_size(mode_btn, 35, 35);
    lv_obj_set_style_radius(mode_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(mode_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(mode_btn, LV_OPA_10, 0);
    lv_obj_set_style_border_width(mode_btn, 0, 0);
    lv_obj_set_style_shadow_width(mode_btn, 0, 0);

    s_mode_icon = lv_label_create(mode_btn);
    lv_label_set_text(s_mode_icon, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(s_mode_icon, COLOR_MUTED, 0);
    lv_obj_set_style_text_font(s_mode_icon, &lv_font_montserrat_14, 0);
    lv_obj_center(s_mode_icon);
    mode_update_icon();
    lv_obj_add_event_cb(mode_btn, mode_btn_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(mode_btn, mode_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(mode_btn, mode_long_press_cb, LV_EVENT_LONG_PRESSED, NULL);

    /* ── 技术参数信息 (右侧 2x2) ── */
    /* 左上: 格式 */
    s_fmt_val = lv_label_create(panel);
    lv_obj_set_pos(s_fmt_val, 50, 4);
    lv_obj_set_style_text_font(s_fmt_val, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_fmt_val, COLOR_ACCENT, 0);
    lv_label_set_text(s_fmt_val, "");

    /* 右上: 采样率 */
    s_sr_val = lv_label_create(panel);
    lv_obj_set_pos(s_sr_val, 105, 4);
    lv_obj_set_style_text_font(s_sr_val, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_sr_val, COLOR_FG, 0);
    lv_label_set_text(s_sr_val, "");

    /* 左下: 声道 */
    s_ch_val = lv_label_create(panel);
    lv_obj_set_pos(s_ch_val, 50, 21);
    lv_obj_set_style_text_font(s_ch_val, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_ch_val, COLOR_FG, 0);
    lv_label_set_text(s_ch_val, "");

    /* 右下: MP3 码率 / FLAC-WAV 位深 */
    s_bit_val = lv_label_create(panel);
    lv_obj_set_pos(s_bit_val, 105, 21);
    lv_obj_set_style_text_font(s_bit_val, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_bit_val, COLOR_FG, 0);
    lv_label_set_text(s_bit_val, "");

    lv_timer_create(fs_sd_monitor_cb, 50, NULL);
    lv_timer_create(song_info_monitor_cb, 500, NULL);
    lv_timer_create(volume_monitor_cb, 200, NULL);
    vol_key_init();

    vol_popup_create();
    bri_draw_create();
}
