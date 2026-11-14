/* 设置页 (APP_MODE_SETTINGS): 白底整屏 lv_list + 固件版本详情页 + WiFi 子流程入口.
 *
 * 它和音乐/小说/菜单一样是"一种模式": 由 app_mode.c 在模式切换的黑场里
 * 调用 ui_settings_build() 建、ui_settings_destroy() 删, 进出都走原有过渡动画.
 * 本文件不碰资源 (音频/封面/蓝牙) 也不碰 NVS —— 那些由 app_mode 的
 * leave/enter 负责.
 *
 * 页面层级 (电源键短按 = 返回上一级; 已经在「设置列表」这一级时退出设置回菜单):
 *
 *   设置列表 ─「固件版本」→ 固件版本详情 ─「检查更新」→ 连接 WiFi ─ 密码(横屏) …
 *      │  │                       ↑                        └→ 正在连接 → 正在检查更新
 *      │  │                       │                                       ↓
 *      │  └───────────────────────┴──────── 版本页 → 下载/写入 → 重启进新固件
 *      └─「自动休眠」→ 休眠时长子页 (小时 0~3 / 分钟 0~60 步进 10 → 保存到 NVS)
 *      └─「自动熄屏」→ 熄屏时长子页 (通用 / 小说 / 歌词 三条分支, 1~10 分钟)
 *      └─「浅睡时长」→ 深睡前先浅睡多久 (0~10 分钟, 0 = 直接深睡)
 *
 * 「检查更新」之后那几页全在 ui_settings_wifi.c 里 (它自己开关 WiFi、自己管返回),
 * 本文件只负责: 进流程前把列表/详情藏起来, 流程结束时把详情页显示回来.
 * 例外是升级成功: 那条路不回来, 释放完 WiFi 直接 esp_restart(). */
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "lvgl.h"

#include "app_mode.h"
#include "autosleep.h"
#include "power_mgr.h"
#include "settings.h"
#include "ui_core.h"
#include "ui_settings.h"
#include "ui_settings_wifi.h"

static const char *TAG = "SETTINGS";

/* ── 白底配色 (本页与其它深色页面不同, 单独一套) ── */
#define SET_BG       lv_color_hex(0xFFFFFF)   /* 页面/列表底色 */
#define SET_TEXT     lv_color_hex(0x111111)   /* 主文字 */
#define SET_MUTED    lv_color_hex(0x888888)   /* 标签/次要文字 */
#define SET_LINE     lv_color_hex(0xE0E0E0)   /* 分隔线 */
#define SET_ARROW    lv_color_hex(0xB0B0B0)   /* 行尾箭头 */
#define SET_BTN_BG   lv_color_hex(0xF5F5F5)   /* 按钮底色 */
#define SET_BTN_BR   lv_color_hex(0xD0D0D0)   /* 按钮描边 */
#define SET_PRESSED  lv_color_hex(0xEFEFEF)   /* 按下态底色 */

/* ── 版面 ── */
#define ROW_H        52                       /* 列表行高 */
#define TITLE_H      48                       /* 详情页顶栏高 */
#define PAD_X        12                       /* 左右留白 */
#define BTN_W        144
#define BTN_H        44
#define BTN_Y        252

/* 自动休眠子页: 两个"下拉"按钮 + 保存 */
#define PICK_BTN_H    38                      /* 下拉按钮高 */
#define PICK_ROW_H    30                      /* 选择弹层的行高 */
#define PICK_CARD_W   132                     /* 选择弹层卡片宽 */

extern const lv_font_t lv_font_global_16;     /* 全局中文字库 */

static lv_obj_t *s_list;                      /* 列表页 (整屏, NULL=未构建) */
static lv_obj_t *s_detail;                    /* 固件版本详情页 (默认隐藏) */
static lv_obj_t *s_sleep;                     /* 自动休眠子页 (默认隐藏) */
static lv_obj_t *s_idle;                      /* 自动熄屏子页 (默认隐藏) */
static lv_obj_t *s_ls;                        /* 浅睡时长子页 (默认隐藏) */

/* 选择弹层要改哪个值: 小时/分钟属于自动休眠, 后三个属于自动熄屏的通用/小说/歌词 */
typedef enum {
    PICK_HOUR = 0,
    PICK_MINUTE,
    PICK_IDLE_COMMON,
    PICK_IDLE_NOVEL,
    PICK_IDLE_LYRICS,
    PICK_LIGHTSLEEP,
} pick_id_t;

/* 自动休眠子页的控件与"编辑中的临时值": 只有点「保存」才落盘 */
static lv_obj_t *s_hour_lbl  = NULL;          /* 小时按钮上的文字 */
static lv_obj_t *s_min_lbl   = NULL;          /* 分钟按钮上的文字 */
static lv_obj_t *s_save_btn  = NULL;          /* 保存 (0 小时 0 分时禁用) */
static lv_obj_t *s_save_lbl  = NULL;          /* 保存按钮上的文字 (禁用态换灰色) */
static int       s_hour_sel  = 1;             /* 0~3 */
static int       s_min_sel   = 0;             /* 0~60, 步进 10 */

/* 自动熄屏子页: 通用 / 小说 / 歌词, 下标与 settings.h 的 SETTINGS_IDLE_* 一致 */
static lv_obj_t *s_idle_lbl[SETTINGS_IDLE_NUM] = { NULL, NULL, NULL };
static int       s_idle_sel[SETTINGS_IDLE_NUM] = { 2, 5, 5 };

/* 浅睡时长子页: 0~10 分钟 (0 = 不浅睡, 直接深睡) */
static lv_obj_t *s_ls_lbl = NULL;
static int       s_ls_sel = LIGHT_SLEEP_MIN_DEFAULT;

/* 选择弹层 (自绘的"下拉"): 白卡 + 若干行, 点行选中, 点卡片外关闭.
 * 背景完全透明 —— 它只负责接"点外面"这一下, 不盖一层灰. */
static lv_obj_t *s_pick      = NULL;
static pick_id_t s_pick_id   = PICK_HOUR;
static int       s_pick_base = 0;             /* 第一项的值 */
static int       s_pick_step = 1;             /* 相邻两项的差 */
static const char *s_pick_fmt = "%d 分钟";    /* 行文字格式 */

static void pick_close(void);                 /* 前向: ui_settings_back 要先关弹层 */
static void idle_ui_refresh(void);            /* 前向: pick_row_cb 要刷新熄屏页 */
static void ls_ui_refresh(void);              /* 前向: pick_row_cb 要刷新浅睡页 */

/* 当前运行镜像的版本/大小: 只在第一次进设置时取一次, 之后直接用缓存 */
static bool     s_info_ready = false;
static char     s_app_ver[40] = "-";          /* "0.7.3" */
static uint32_t s_app_len = 0;                /* 镜像字节数, 0=没取到 */
static char     s_app_len_str[24] = "-";      /* "2,316,880" */
static char     s_app_mb_str[24] = "";        /* "2.21 MB" */

/* ── 小工具 ── */

/* 2316880 -> "2,316,880" */
static void fmt_thousands(uint32_t v, char *out, size_t cap)
{
    char raw[16];
    snprintf(raw, sizeof raw, "%lu", (unsigned long)v);
    size_t n = strlen(raw);
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        if (i > 0 && (n - i) % 3 == 0 && w + 1 < cap) out[w++] = ',';
        if (w + 1 < cap) out[w++] = raw[i];
    }
    out[w] = '\0';
}

/* 2316880 -> "2.21 MB" (四舍五入到 0.01MB) */
static void fmt_mb(uint32_t v, char *out, size_t cap)
{
    uint64_t cs = ((uint64_t)v * 100 + (1u << 19)) >> 20;   /* 以 0.01MB 为单位 */
    snprintf(out, cap, "%lu.%02lu MB",
             (unsigned long)(cs / 100), (unsigned long)(cs % 100));
}

/* 当前运行镜像在 flash 上的真实长度 (= 烧进去的 .bin 字节数), 失败返回 0.
 *
 * 本来想直接调 esp_image_verify() 拿 meta.image_len, 但它在运行时会把整幅
 * 镜像 mmap 进 DROM 窗口去核 SHA256 —— 本工程字库已经吃掉 50 页, 64 页的
 * MMU 页池只剩个位数, 于是必然失败 (实测 "No free MMU pages are available",
 * ESP_ERR_NO_MEM).
 * 所以这里只读镜像头 + 各段头, 按 bootloader 的规则自己算长度:
 *   24(镜像头) + Σ(8+段长度) + 1(校验和) → 补齐到 16 字节 → +32(SHA256 摘要)
 * 实测结果与 build\fw\app.bin 逐字节一致 (0x236580 = 2,319,744), 且总共
 * 只读一百多字节, 不用等. */
static uint32_t running_image_len(void)
{
    const esp_partition_t *p = esp_ota_get_running_partition();
    if (!p) return 0;

    esp_image_header_t hdr;
    if (esp_partition_read(p, 0, &hdr, sizeof hdr) != ESP_OK) return 0;
    if (hdr.magic != ESP_IMAGE_HEADER_MAGIC) {
        ESP_LOGW(TAG, "镜像头 magic 不对: 0x%02x", hdr.magic);
        return 0;
    }

    uint32_t off = sizeof hdr;
    for (int i = 0; i < hdr.segment_count; i++) {
        esp_image_segment_header_t sh;
        if (esp_partition_read(p, off, &sh, sizeof sh) != ESP_OK) return 0;
        off += sizeof sh + sh.data_len;
        if (off > p->size) {
            ESP_LOGW(TAG, "段表越界 (段%d, off=%lu)", i, (unsigned long)off);
            return 0;
        }
    }

    off += 1;                          /* 8 bit 校验和 */
    off = (off + 15) & ~15u;           /* bootloader 把镜像补齐到 16 字节 */
    if (hdr.hash_appended) off += 32;  /* 结尾追加的 SHA256 摘要 */
    return off;
}

/* 首次进设置时把版本/大小取好并格式化 (之后只读缓存) */
static void ensure_info(void)
{
    if (s_info_ready) return;
    s_info_ready = true;

    const esp_app_desc_t *d = esp_app_get_description();
    if (d) snprintf(s_app_ver, sizeof s_app_ver, "%.32s", d->version);

    s_app_len = running_image_len();
    if (s_app_len) {
        fmt_thousands(s_app_len, s_app_len_str, sizeof s_app_len_str);
        fmt_mb(s_app_len, s_app_mb_str, sizeof s_app_mb_str);
    }
    ESP_LOGI(TAG, "APP 版本 %s, 大小 %s 字节 %s", s_app_ver, s_app_len_str, s_app_mb_str);
}

/* 一整页白底容器 (172x320, 不滚动) */
static lv_obj_t *page_create(void)
{
    lv_obj_t *o = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(o);                    /* 去掉主题给的边距/边框/阴影 */
    lv_obj_set_size(o, TFT_HOR_RES, TFT_VER_RES);
    lv_obj_set_pos(o, 0, 0);
    lv_obj_set_style_bg_color(o, SET_BG, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

/* 在 parent 上放一个中文标签 (w=0 表示按文字自适应宽度) */
static lv_obj_t *make_label(lv_obj_t *parent, const char *txt, lv_color_t color,
                            lv_coord_t x, lv_coord_t y, lv_coord_t w,
                            lv_text_align_t align)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &lv_font_global_16, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_pos(l, x, y);
    if (w) lv_obj_set_width(l, w);
    if (align != LV_TEXT_ALIGN_LEFT) lv_obj_set_style_text_align(l, align, 0);
    return l;
}

/* ── 列表页 / 详情页 切换 ── */

/* 当前停在哪一级: 电源键短按要知道"还能不能再往上退" */
typedef enum {
    PG_LIST = 0,      /* 根: 再返回就是退出设置 */
    PG_VERSION,       /* 固件版本详情 */
    PG_SLEEP,         /* 自动休眠子页 */
    PG_IDLE,          /* 自动熄屏子页 */
    PG_LIGHTSLEEP,    /* 浅睡时长子页 */
} page_t;

static page_t s_page = PG_LIST;

/* 五页互斥显示: 先全隐, 再放一个出来 */
static void hide_all(void)
{
    if (s_list)   lv_obj_add_flag(s_list,   LV_OBJ_FLAG_HIDDEN);
    if (s_detail) lv_obj_add_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
    if (s_sleep)  lv_obj_add_flag(s_sleep,  LV_OBJ_FLAG_HIDDEN);
    if (s_idle)   lv_obj_add_flag(s_idle,   LV_OBJ_FLAG_HIDDEN);
    if (s_ls)     lv_obj_add_flag(s_ls,     LV_OBJ_FLAG_HIDDEN);
}

static void list_show(void)
{
    hide_all();
    if (s_list) lv_obj_clear_flag(s_list, LV_OBJ_FLAG_HIDDEN);
    s_page = PG_LIST;
}

static void detail_show(void)
{
    if (!s_detail) return;
    hide_all();
    lv_obj_move_foreground(s_detail);              /* 压住列表页 */
    lv_obj_clear_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
    s_page = PG_VERSION;
}

/* 点「固件版本」行: 进详情页 */
static void version_row_cb(lv_event_t *e)
{
    (void)e;
    detail_show();
    power_mgr_notify_activity();                   /* 点按也算操作, 续期熄屏计时 */
}

/* 点「自动休眠」行: 进休眠时长子页 (定义在后面, 这里先声明) */
static void sleep_show(void);
static void sleep_row_cb(lv_event_t *e)
{
    (void)e;
    sleep_show();
    power_mgr_notify_activity();
}

/* 点「自动熄屏」行: 进熄屏时长子页 */
static void idle_row_cb(lv_event_t *e);

/* 点「浅睡时长」行: 进浅睡时长子页 */
static void ls_row_cb(lv_event_t *e);

/* 详情页左上角「‹」: 回列表 (模式本身不动) */
static void back_cb(lv_event_t *e)
{
    (void)e;
    list_show();
    power_mgr_notify_activity();
}

/* 「检查更新」: 进 WiFi 子流程 (列表/详情先藏起来, 流程结束时再显示回来) */
static void check_update_cb(lv_event_t *e)
{
    (void)e;
    power_mgr_notify_activity();

    if (ui_wifi_active()) return;
    if (!ui_wifi_enter()) {
        ESP_LOGW(TAG, "进入 WiFi 流程失败 (上一次会话可能还在释放)");
        return;
    }
    hide_all();                                    /* 列表/详情/休眠页都先藏起来 */
}

/* WiFi 流程结束 (扫描页/检查更新页返回, 资源已释放): 回到固件版本详情 */
static void wifi_flow_end_cb(void)
{
    detail_show();
    power_mgr_notify_activity();
}

/* 电源键短按: 返回上一级; 已经在设置列表这一级 → 退出设置 (回菜单).
 * 选择弹层开着时先关弹层 (这一按只用来退弹层, 不退页面).
 * 扫描/连接/释放期间由 ui_wifi_back() 自己拦掉 (按了没反应). */
void ui_settings_back(void)
{
    if (s_pick) {                                  /* 自绘下拉开着 */
        pick_close();
        power_mgr_notify_activity();
        return;
    }

    if (ui_wifi_active()) {
        ui_wifi_back();
        return;
    }

    if (s_page == PG_VERSION || s_page == PG_SLEEP || s_page == PG_IDLE ||
        s_page == PG_LIGHTSLEEP) {
        list_show();
        power_mgr_notify_activity();
        return;
    }

    ESP_LOGI(TAG, "设置列表: 退出设置, 回主菜单");
    app_mode_request(APP_MODE_MENU);
}

/* 列表行: 文字 + 行尾箭头 (固件版本 / 自动休眠 共用一套样式) */
static void add_row(lv_obj_t *list, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_list_add_btn(list, NULL, text);
    lv_obj_set_size(btn, TFT_HOR_RES, ROW_H);
    lv_obj_set_style_bg_color(btn, SET_BG, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, SET_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, 0, 0);
    lv_obj_set_style_border_side(btn, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(btn, SET_LINE, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_left(btn, 14, 0);
    lv_obj_set_style_pad_right(btn, 12, 0);
    /* 行内是 flex row: 文字标签 flex_grow=1 撑开, 箭头自然贴右边 */
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *txt = lv_obj_get_child(btn, 0);
    if (txt) {
        lv_label_set_long_mode(txt, LV_LABEL_LONG_CLIP);   /* 不滚动 */
        lv_obj_set_style_text_font(txt, &lv_font_global_16, 0);
        lv_obj_set_style_text_color(txt, SET_TEXT, 0);
    }

    lv_obj_t *arrow = lv_label_create(btn);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(arrow, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(arrow, SET_ARROW, 0);

    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
}

/* ── 列表页 ── */
static void list_build(void)
{
    s_list = page_create();

    lv_obj_t *list = lv_list_create(s_list);
    lv_obj_set_size(list, TFT_HOR_RES, TFT_VER_RES);
    lv_obj_set_pos(list, 0, 0);
    lv_obj_set_style_bg_color(list, SET_BG, 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(list, 0, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, 0, 0);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

    add_row(list, "固件版本", version_row_cb);
    add_row(list, "自动休眠", sleep_row_cb);
    add_row(list, "自动熄屏", idle_row_cb);
    add_row(list, "浅睡时长", ls_row_cb);
}

/* 子页顶栏: 返回按钮 + 居中标题 + 1px 分隔线 (固件版本 / 自动休眠 共用) */
static void title_bar_create(lv_obj_t *page, const char *title)
{
    lv_obj_t *back = lv_btn_create(page);
    lv_obj_set_pos(back, 0, 0);
    lv_obj_set_size(back, TITLE_H, TITLE_H);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_set_style_shadow_width(back, 0, 0);
    lv_obj_set_style_radius(back, 0, 0);
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_lbl = lv_label_create(back);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(back_lbl, SET_TEXT, 0);
    lv_obj_center(back_lbl);

    make_label(page, title, SET_TEXT, TITLE_H, 15,
               TFT_HOR_RES - 2 * TITLE_H, LV_TEXT_ALIGN_CENTER);

    lv_obj_t *line = lv_obj_create(page);
    lv_obj_remove_style_all(line);
    lv_obj_set_pos(line, 0, TITLE_H);
    lv_obj_set_size(line, TFT_HOR_RES, 1);
    lv_obj_set_style_bg_color(line, SET_LINE, 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
}

/* ── 固件版本详情页 ── */
static void detail_build(void)
{
    s_detail = page_create();
    lv_obj_add_flag(s_detail, LV_OBJ_FLAG_HIDDEN);   /* 进来先停在列表页 */

    title_bar_create(s_detail, "固件版本");

    /* 版本 */
    make_label(s_detail, "版本", SET_MUTED, PAD_X, 62, 0, LV_TEXT_ALIGN_LEFT);
    make_label(s_detail, s_app_ver, SET_TEXT, PAD_X, 84, 0, LV_TEXT_ALIGN_LEFT);

    /* APP 大小: 字节数 + MB 两个单位都给出 */
    make_label(s_detail, "APP 大小", SET_MUTED, PAD_X, 120, 0, LV_TEXT_ALIGN_LEFT);
    char buf[40];
    snprintf(buf, sizeof buf, "%s 字节", s_app_len_str);
    make_label(s_detail, buf, SET_TEXT, PAD_X, 142, 0, LV_TEXT_ALIGN_LEFT);
    if (s_app_mb_str[0]) {
        make_label(s_detail, s_app_mb_str, SET_MUTED, PAD_X, 166, 0, LV_TEXT_ALIGN_LEFT);
    }

    /* 检查更新: 进 ui_settings_wifi.c 的子流程 (连 WiFi → 查版本 → 下载/写入) */
    lv_obj_t *btn = lv_btn_create(s_detail);
    lv_obj_set_pos(btn, (TFT_HOR_RES - BTN_W) / 2, BTN_Y);
    lv_obj_set_size(btn, BTN_W, BTN_H);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_bg_color(btn, SET_BTN_BG, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, SET_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, SET_BTN_BR, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, check_update_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, "检查更新");
    lv_obj_set_style_text_font(btn_lbl, &lv_font_global_16, 0);
    lv_obj_set_style_text_color(btn_lbl, SET_TEXT, 0);
    lv_obj_center(btn_lbl);
}

/* ── 自动休眠子页 ──
 *
 * 两个"下拉": 一行按钮写着当前值 + ▾, 点开是居中的白卡弹层列出可选值.
 * 为什么不用 lv_dropdown: lv_conf.h 里 dropdown/roller 都是 0 (开了要全量重编 LVGL),
 * 而且 LVGL 8 的下拉列表固定往下展开 —— 分钟这档有 7 项, 在 172x320 上必然探出屏幕.
 * 自绘弹层还能让"点哪里关、点哪里选"是确定的.
 *
 * 编辑中的值只存 s_hour_sel / s_min_sel, 点「保存」才落 NVS: 半路按返回不留痕. */

/* 两颗按钮上的文字 + 保存按钮的可用性 (0 小时 0 分没有意义, 直接不可点) */
static void sleep_ui_refresh(void)
{
    if (s_hour_lbl) lv_label_set_text_fmt(s_hour_lbl, "%d 小时", s_hour_sel);
    if (s_min_lbl)  lv_label_set_text_fmt(s_min_lbl,  "%d 分钟", s_min_sel);

    const bool zero = (s_hour_sel == 0 && s_min_sel == 0);

    if (s_save_btn) {
        if (zero) lv_obj_add_state(s_save_btn, LV_STATE_DISABLED);
        else      lv_obj_clear_state(s_save_btn, LV_STATE_DISABLED);
    }
    if (s_save_lbl) {                        /* 禁用态自己不能变灰: 显式换文字色 */
        lv_obj_set_style_text_color(s_save_lbl, zero ? SET_MUTED : SET_TEXT, 0);
    }
}

static void pick_close(void)
{
    if (s_pick) { lv_obj_del(s_pick); s_pick = NULL; }
}

/* 点卡片外: 关掉弹层, 不改任何值 */
static void pick_overlay_cb(lv_event_t *e)
{
    (void)e;
    pick_close();
    power_mgr_notify_activity();
}

/* 点某一项: 更新编辑值, 关弹层 (值不落盘, 等「保存」) */
static void pick_row_cb(lv_event_t *e)
{
    int v = (int)(intptr_t)lv_event_get_user_data(e);

    switch (s_pick_id) {
    case PICK_HOUR:        s_hour_sel = v;            break;
    case PICK_MINUTE:      s_min_sel  = v;            break;
    case PICK_IDLE_COMMON: s_idle_sel[SETTINGS_IDLE_COMMON] = v; break;
    case PICK_IDLE_NOVEL:  s_idle_sel[SETTINGS_IDLE_NOVEL]  = v; break;
    case PICK_IDLE_LYRICS: s_idle_sel[SETTINGS_IDLE_LYRICS] = v; break;
    case PICK_LIGHTSLEEP:  s_ls_sel = v; break;
    }

    pick_close();
    switch (s_pick_id) {
    case PICK_HOUR:
    case PICK_MINUTE:     sleep_ui_refresh(); break;
    case PICK_LIGHTSLEEP: ls_ui_refresh();    break;
    default:              idle_ui_refresh();  break;
    }
    power_mgr_notify_activity();
}

/* 打开选择弹层.
 * id=改哪个值; sel=当前值; base/step/count=候选值 (base + i*step); fmt=行文字格式.
 * 自动休眠: (0,1,4,"%d 小时") 与 (0,10,7,"%d 分钟");
 * 自动熄屏: 三条都是 (1,1,10,"%d 分钟"). */
static void pick_open(pick_id_t id, int sel, int base, int step, int count, const char *fmt)
{
    pick_close();
    s_pick_id   = id;
    s_pick_base = base;
    s_pick_step = step;
    s_pick_fmt  = fmt;

    /* 10 项时行高收到 26, 否则 10*30+8 = 308 会把卡片顶到屏幕外 */
    const int row_h  = (count > 7) ? 26 : PICK_ROW_H;
    const int card_h = count * row_h + 8;

    /* 透明遮罩: 只负责"点外面 = 取消", 不再压一层半透明黑 —— 设置页本来就是白底,
     * 盖灰会让弹层后整页变暗, 看着像换了张页面. */
    s_pick = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_pick);
    lv_obj_set_pos(s_pick, 0, 0);
    lv_obj_set_size(s_pick, TFT_HOR_RES, TFT_VER_RES);
    lv_obj_set_style_bg_opa(s_pick, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(s_pick, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_pick, pick_overlay_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *card = lv_obj_create(s_pick);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, PICK_CARD_W, card_h);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, SET_BG, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_border_color(card, SET_LINE, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_shadow_width(card, 14, 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_30, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    /* 卡片本身也接点击: 落在行间的缝里时不要去关弹层 */
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < count; i++) {
        const int v = base + i * step;

        lv_obj_t *row = lv_btn_create(card);
        lv_obj_set_pos(row, 4, 4 + i * row_h);
        lv_obj_set_size(row, PICK_CARD_W - 8, row_h);
        lv_obj_set_style_radius(row, 0, 0);
        /* 当前值那一行用浅底标出来, 不用符号: 全局 16 号中文字库里没有 LV_SYMBOL_* */
        lv_obj_set_style_bg_color(row, (v == sel) ? SET_PRESSED : SET_BG, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, SET_PRESSED, LV_STATE_PRESSED);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(row, SET_LINE, 0);
        lv_obj_set_style_border_width(row, (i == count - 1) ? 0 : 1, 0);
        lv_obj_add_event_cb(row, pick_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)v);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text_fmt(lbl, fmt, v);
        lv_obj_set_style_text_font(lbl, &lv_font_global_16, 0);
        lv_obj_set_style_text_color(lbl, SET_TEXT, 0);
        lv_obj_center(lbl);
    }
}

static void hour_btn_cb(lv_event_t *e)
{
    (void)e;
    pick_open(PICK_HOUR, s_hour_sel, 0, 1, 4, "%d 小时");
    power_mgr_notify_activity();
}

static void min_btn_cb(lv_event_t *e)
{
    (void)e;
    pick_open(PICK_MINUTE, s_min_sel, 0, 10, 7, "%d 分钟");
    power_mgr_notify_activity();
}

/* "下拉"按钮: 左边当前值, 右边 ▾; out_lbl 带回文字标签供刷新.
 * cb 的 user_data 由调用方给 (自动熄屏三个按钮共用同一个回调, 靠它区分分支). */
static lv_obj_t *make_picker_btn(lv_obj_t *parent, lv_coord_t y, lv_event_cb_t cb,
                                 void *user, lv_obj_t **out_lbl)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_pos(btn, PAD_X, y);
    lv_obj_set_size(btn, TFT_HOR_RES - 2 * PAD_X, PICK_BTN_H);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_bg_color(btn, SET_BG, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, SET_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, SET_BTN_BR, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_obj_set_style_text_font(lbl, &lv_font_global_16, 0);
    lv_obj_set_style_text_color(lbl, SET_TEXT, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 10, 0);

    lv_obj_t *arrow = lv_label_create(btn);
    lv_label_set_text(arrow, LV_SYMBOL_DOWN);
    lv_obj_set_style_text_font(arrow, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(arrow, SET_ARROW, 0);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -10, 0);

    if (out_lbl) *out_lbl = lbl;
    return btn;
}

/* 保存: 写 NVS; 自动休眠开着的话, autosleep 会按新时长重新计时 */
static void sleep_save_cb(lv_event_t *e)
{
    (void)e;
    if (s_hour_sel == 0 && s_min_sel == 0) return;   /* 按钮已禁用, 这里再兜一次 */

    const int total = s_hour_sel * 60 + s_min_sel;
    autosleep_set_minutes(total);
    ESP_LOGI(TAG, "自动休眠时长: %d 小时 %d 分 (共 %d 分钟)", s_hour_sel, s_min_sel, total);

    list_show();
    power_mgr_notify_activity();
}

/* 进子页: 每次都从运行时的设定值重取, 半路改过但没保存的值不保留 */
static void sleep_show(void)
{
    if (!s_sleep) return;

    const int total = autosleep_total_minutes();
    s_hour_sel = total / 60;
    s_min_sel  = (total % 60) / 10 * 10;
    if (s_hour_sel > 3) s_hour_sel = 3;
    sleep_ui_refresh();

    hide_all();
    lv_obj_move_foreground(s_sleep);
    lv_obj_clear_flag(s_sleep, LV_OBJ_FLAG_HIDDEN);
    s_page = PG_SLEEP;
}

/* ── 自动休眠子页 ── */
static void sleep_page_build(void)
{
    s_sleep = page_create();
    lv_obj_add_flag(s_sleep, LV_OBJ_FLAG_HIDDEN);

    title_bar_create(s_sleep, "自动休眠");

    make_label(s_sleep, "小时", SET_MUTED, PAD_X, 56, 0, LV_TEXT_ALIGN_LEFT);
    make_picker_btn(s_sleep, 78, hour_btn_cb, NULL, &s_hour_lbl);

    make_label(s_sleep, "分钟", SET_MUTED, PAD_X, 122, 0, LV_TEXT_ALIGN_LEFT);
    make_picker_btn(s_sleep, 144, min_btn_cb, NULL, &s_min_lbl);

    /* 保存 (0 小时 0 分时禁用) */
    s_save_btn = lv_btn_create(s_sleep);
    lv_obj_set_pos(s_save_btn, (TFT_HOR_RES - BTN_W) / 2, 206);
    lv_obj_set_size(s_save_btn, BTN_W, BTN_H);
    lv_obj_set_style_radius(s_save_btn, 6, 0);
    lv_obj_set_style_bg_color(s_save_btn, SET_BTN_BG, 0);
    lv_obj_set_style_bg_opa(s_save_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_save_btn, SET_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(s_save_btn, SET_BTN_BR, 0);
    lv_obj_set_style_border_width(s_save_btn, 1, 0);
    lv_obj_set_style_shadow_width(s_save_btn, 0, 0);
    lv_obj_add_event_cb(s_save_btn, sleep_save_cb, LV_EVENT_CLICKED, NULL);

    s_save_lbl = lv_label_create(s_save_btn);
    lv_label_set_text(s_save_lbl, "保存");
    lv_obj_set_style_text_font(s_save_lbl, &lv_font_global_16, 0);
    lv_obj_set_style_text_color(s_save_lbl, SET_TEXT, 0);
    lv_obj_center(s_save_lbl);

    make_label(s_sleep, "保存后按新时长重新计时", SET_MUTED, PAD_X, 262,
               TFT_HOR_RES - 2 * PAD_X, LV_TEXT_ALIGN_CENTER);
}

/* ── 自动熄屏子页 ──
 *
 * 三条: 通用 / 小说 / 歌词, 1~10 分钟. 通用兜住菜单、设置、音乐封面以及以后新加的
 * 任何模式, 小说和歌词只是两条覆盖 —— 想改全局就改通用, 不需要"跟随通用"这种选项.
 *
 * 和自动休眠一样, 编辑值只在内存, 点「保存」才落 NVS; 但 1~10 全都合法,
 * 保存按钮没有禁用态. */

static void idle_ui_refresh(void)
{
    for (int i = 0; i < SETTINGS_IDLE_NUM; i++) {
        if (s_idle_lbl[i]) lv_label_set_text_fmt(s_idle_lbl[i], "%d 分钟", s_idle_sel[i]);
    }
}

/* 三个按钮共用一个回调: user_data 带回分支号 */
static void idle_btn_cb(lv_event_t *e)
{
    int br = (int)(intptr_t)lv_event_get_user_data(e);
    if (br < 0 || br >= SETTINGS_IDLE_NUM) return;

    pick_id_t id = (br == SETTINGS_IDLE_NOVEL)  ? PICK_IDLE_NOVEL  :
                   (br == SETTINGS_IDLE_LYRICS) ? PICK_IDLE_LYRICS : PICK_IDLE_COMMON;

    pick_open(id, s_idle_sel[br], IDLE_MIN_MIN, 1,
              IDLE_MIN_MAX - IDLE_MIN_MIN + 1, "%d 分钟");
    power_mgr_notify_activity();
}

/* 保存: 三条一起落到缓存 + NVS, 立刻生效 (下一次 1 秒轮询就用新值) */
static void idle_save_cb(lv_event_t *e)
{
    (void)e;
    for (int i = 0; i < SETTINGS_IDLE_NUM; i++) {
        power_mgr_idle_set(i, s_idle_sel[i]);
    }
    ESP_LOGI(TAG, "自动熄屏: 通用 %d / 小说 %d / 歌词 %d 分钟",
             s_idle_sel[SETTINGS_IDLE_COMMON], s_idle_sel[SETTINGS_IDLE_NOVEL],
             s_idle_sel[SETTINGS_IDLE_LYRICS]);

    list_show();
    power_mgr_notify_activity();
}

/* 进子页: 从运行时缓存取当前值 (开机已从 NVS 灌入, 保存时同步) */
static void idle_show(void)
{
    if (!s_idle) return;

    for (int i = 0; i < SETTINGS_IDLE_NUM; i++) {
        s_idle_sel[i] = power_mgr_idle_minutes(i);
    }
    idle_ui_refresh();

    hide_all();
    lv_obj_move_foreground(s_idle);
    lv_obj_clear_flag(s_idle, LV_OBJ_FLAG_HIDDEN);
    s_page = PG_IDLE;
}

static void idle_row_cb(lv_event_t *e)
{
    (void)e;
    idle_show();
    power_mgr_notify_activity();
}

static void idle_page_build(void)
{
    s_idle = page_create();
    lv_obj_add_flag(s_idle, LV_OBJ_FLAG_HIDDEN);

    title_bar_create(s_idle, "自动熄屏");

    static const char *const NAME[SETTINGS_IDLE_NUM] = { "通用", "小说", "歌词" };
    /* 名字不能叫 BTN_Y / LBL_Y: 上面有同名宏, 预处理器会把数组名换成数字 */
    static const lv_coord_t CAP_Y[SETTINGS_IDLE_NUM] = { 50, 112, 174 };
    static const lv_coord_t PICK_Y[SETTINGS_IDLE_NUM] = { 68, 130, 192 };

    for (int i = 0; i < SETTINGS_IDLE_NUM; i++) {
        make_label(s_idle, NAME[i], SET_MUTED, PAD_X, CAP_Y[i], 0, LV_TEXT_ALIGN_LEFT);
        make_picker_btn(s_idle, PICK_Y[i], idle_btn_cb, (void *)(intptr_t)i, &s_idle_lbl[i]);
    }

    /* 保存 (三条都合法, 始终可点) */
    lv_obj_t *save = lv_btn_create(s_idle);
    lv_obj_set_pos(save, (TFT_HOR_RES - BTN_W) / 2, 240);
    lv_obj_set_size(save, BTN_W, BTN_H);
    lv_obj_set_style_radius(save, 6, 0);
    lv_obj_set_style_bg_color(save, SET_BTN_BG, 0);
    lv_obj_set_style_bg_opa(save, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(save, SET_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(save, SET_BTN_BR, 0);
    lv_obj_set_style_border_width(save, 1, 0);
    lv_obj_set_style_shadow_width(save, 0, 0);
    lv_obj_add_event_cb(save, idle_save_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *sl = lv_label_create(save);
    lv_label_set_text(sl, "保存");
    lv_obj_set_style_text_font(sl, &lv_font_global_16, 0);
    lv_obj_set_style_text_color(sl, SET_TEXT, 0);
    lv_obj_center(sl);
}

/* ── 浅睡时长子页 ──
 *
 * 一行: 0~10 分钟. **0 = 不浅睡、直接深睡** (浅睡之前的老行为), 1~10 = 淡出后
 * 先浅睡 N 分钟再深睡. 只影响"短按(蓝牙断开)/无操作超时/息屏期蓝牙断开"三条入口,
 * 自动休眠到点与低电量关机仍然直接深睡. */

static void ls_ui_refresh(void)
{
    if (s_ls_lbl) lv_label_set_text_fmt(s_ls_lbl, "%d 分钟", s_ls_sel);
}

static void ls_btn_cb(lv_event_t *e)
{
    (void)e;
    pick_open(PICK_LIGHTSLEEP, s_ls_sel, LIGHT_SLEEP_MIN_MIN, 1,
              LIGHT_SLEEP_MIN_MAX - LIGHT_SLEEP_MIN_MIN + 1, "%d 分钟");
    power_mgr_notify_activity();
}

static void ls_save_cb(lv_event_t *e)
{
    (void)e;
    power_mgr_light_sleep_set_minutes(s_ls_sel);
    ESP_LOGI(TAG, "浅睡时长: %d 分钟%s", s_ls_sel, s_ls_sel ? "" : " (直接深睡)");

    list_show();
    power_mgr_notify_activity();
}

/* 进子页: 从运行时缓存取当前值 (开机已从 NVS 灌入, 保存时同步) */
static void ls_show(void)
{
    if (!s_ls) return;

    s_ls_sel = power_mgr_light_sleep_minutes();
    ls_ui_refresh();

    hide_all();
    lv_obj_move_foreground(s_ls);
    lv_obj_clear_flag(s_ls, LV_OBJ_FLAG_HIDDEN);
    s_page = PG_LIGHTSLEEP;
}

static void ls_row_cb(lv_event_t *e)
{
    (void)e;
    ls_show();
    power_mgr_notify_activity();
}

static void ls_page_build(void)
{
    s_ls = page_create();
    lv_obj_add_flag(s_ls, LV_OBJ_FLAG_HIDDEN);

    title_bar_create(s_ls, "浅睡时长");

    make_label(s_ls, "浅睡", SET_MUTED, PAD_X, 56, 0, LV_TEXT_ALIGN_LEFT);
    make_picker_btn(s_ls, 78, ls_btn_cb, NULL, &s_ls_lbl);

    /* 说明: 0 的含义容易误会, 直接写在页面上.
     * 三行短句 (每行 <= 9 个汉字), 免得折行后压到下面的保存按钮 */
    make_label(s_ls, "0 = 直接深睡\n1~10 分钟先浅睡\n按电源键可唤醒",
               SET_MUTED, PAD_X, 118, TFT_HOR_RES - 2 * PAD_X, LV_TEXT_ALIGN_LEFT);

    /* 保存 (0~10 都合法, 始终可点) */
    lv_obj_t *save = lv_btn_create(s_ls);
    lv_obj_set_pos(save, (TFT_HOR_RES - BTN_W) / 2, 226);
    lv_obj_set_size(save, BTN_W, BTN_H);
    lv_obj_set_style_radius(save, 6, 0);
    lv_obj_set_style_bg_color(save, SET_BTN_BG, 0);
    lv_obj_set_style_bg_opa(save, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(save, SET_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(save, SET_BTN_BR, 0);
    lv_obj_set_style_border_width(save, 1, 0);
    lv_obj_set_style_shadow_width(save, 0, 0);
    lv_obj_add_event_cb(save, ls_save_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *sl = lv_label_create(save);
    lv_label_set_text(sl, "保存");
    lv_obj_set_style_text_font(sl, &lv_font_global_16, 0);
    lv_obj_set_style_text_color(sl, SET_TEXT, 0);
    lv_obj_center(sl);
}

/* ── 对外接口 ── */
void ui_settings_build(void)
{
    if (s_list) return;            /* 幂等: 已建好就不重建 */

    ensure_info();                 /* 版本/大小只取一次 */
    list_build();
    detail_build();
    sleep_page_build();
    idle_page_build();
    ls_page_build();
    ui_wifi_set_end_cb(wifi_flow_end_cb);   /* WiFi 子流程结束时回到详情页 */
    s_page = PG_LIST;

    lv_obj_move_foreground(s_list);   /* 压住常驻的音量弹窗 */
    ESP_LOGI(TAG, "设置页已构建");
}

void ui_settings_destroy(void)
{
    /* 先收 WiFi 子流程: 它可能停在横屏密码页, 也可能还有会话在释放.
     * 必须在删列表/详情之前做 —— 它要把屏幕旋转恢复成竖屏. */
    ui_wifi_destroy();

    pick_close();                     /* 选择弹层挂在屏幕上, 必须单独删 */
    if (s_sleep)  { lv_obj_del(s_sleep);  s_sleep  = NULL; }
    if (s_idle)   { lv_obj_del(s_idle);   s_idle   = NULL; }
    if (s_ls)     { lv_obj_del(s_ls);     s_ls     = NULL; }
    if (s_detail) { lv_obj_del(s_detail); s_detail = NULL; }
    if (s_list)   { lv_obj_del(s_list);   s_list   = NULL; }

    s_hour_lbl = s_min_lbl = s_save_btn = s_save_lbl = NULL;
    s_ls_lbl = NULL;
    for (int i = 0; i < SETTINGS_IDLE_NUM; i++) s_idle_lbl[i] = NULL;
    s_page = PG_LIST;
    ESP_LOGI(TAG, "设置页已销毁");
}
