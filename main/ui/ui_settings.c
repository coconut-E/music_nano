/* 设置页 (APP_MODE_SETTINGS): 白底整屏 lv_list + 固件版本详情页.
 *
 * 它和音乐/小说/菜单一样是"一种模式": 由 app_mode.c 在模式切换的黑场里
 * 调用 ui_settings_build() 建、ui_settings_destroy() 删, 进出都走原有过渡动画.
 * 本文件不碰资源 (音频/封面/蓝牙) 也不碰 NVS —— 那些由 app_mode 的
 * leave/enter 负责.
 *
 * 列表目前只有一项「固件版本」: 点进去看当前运行 APP 的版本号和镜像大小,
 * 下面一个「检查更新」按钮. 按钮按需求先占位 —— 点了没有任何反应 (只打日志),
 * 联网 OTA 做好了再接到这里. */
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "lvgl.h"

#include "power_mgr.h"
#include "ui_core.h"
#include "ui_settings.h"

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

extern const lv_font_t lv_font_global_16;     /* 全局中文字库 */

static lv_obj_t *s_list;                      /* 列表页 (整屏, NULL=未构建) */
static lv_obj_t *s_detail;                    /* 固件版本详情页 (默认隐藏) */

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

static void list_show(void)
{
    if (s_list)   lv_obj_clear_flag(s_list, LV_OBJ_FLAG_HIDDEN);
    if (s_detail) lv_obj_add_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
}

static void detail_show(void)
{
    if (!s_detail) return;
    lv_obj_move_foreground(s_detail);              /* 压住列表页 */
    lv_obj_clear_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
    if (s_list) lv_obj_add_flag(s_list, LV_OBJ_FLAG_HIDDEN);
}

/* 点「固件版本」行: 进详情页 */
static void version_row_cb(lv_event_t *e)
{
    (void)e;
    detail_show();
    power_mgr_notify_activity();                   /* 点按也算操作, 续期熄屏计时 */
}

/* 详情页左上角「‹」: 回列表 (模式本身不动) */
static void back_cb(lv_event_t *e)
{
    (void)e;
    list_show();
    power_mgr_notify_activity();
}

/* 「检查更新」: 按需求先占位 —— 点了没有任何反应, 只留一条日志 */
static void check_update_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "检查更新: 未实现 (点击无响应)");
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

    /* 唯一一行: 固件版本  › */
    lv_obj_t *btn = lv_list_add_btn(list, NULL, "固件版本");
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

    lv_obj_add_event_cb(btn, version_row_cb, LV_EVENT_CLICKED, NULL);
}

/* ── 固件版本详情页 ── */
static void detail_build(void)
{
    s_detail = page_create();
    lv_obj_add_flag(s_detail, LV_OBJ_FLAG_HIDDEN);   /* 进来先停在列表页 */

    /* 顶栏: 返回 + 标题 + 1px 分隔线 */
    lv_obj_t *back = lv_btn_create(s_detail);
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

    make_label(s_detail, "固件版本", SET_TEXT, TITLE_H, 15,
               TFT_HOR_RES - 2 * TITLE_H, LV_TEXT_ALIGN_CENTER);

    lv_obj_t *line = lv_obj_create(s_detail);
    lv_obj_remove_style_all(line);
    lv_obj_set_pos(line, 0, TITLE_H);
    lv_obj_set_size(line, TFT_HOR_RES, 1);
    lv_obj_set_style_bg_color(line, SET_LINE, 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);

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

    /* 检查更新 (占位: 点了没反应) */
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

/* ── 对外接口 ── */
void ui_settings_build(void)
{
    if (s_list) return;            /* 幂等: 已建好就不重建 */

    ensure_info();                 /* 版本/大小只取一次 */
    list_build();
    detail_build();

    lv_obj_move_foreground(s_list);   /* 压住常驻的音量弹窗 */
    ESP_LOGI(TAG, "设置页已构建");
}

void ui_settings_destroy(void)
{
    if (s_detail) { lv_obj_del(s_detail); s_detail = NULL; }
    if (s_list)   { lv_obj_del(s_list);   s_list   = NULL; }
    ESP_LOGI(TAG, "设置页已销毁");
}
