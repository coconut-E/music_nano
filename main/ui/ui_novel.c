#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "settings.h"
#include "sys_monitor.h"
#include "atomic_utils.h"
#include "ui_core.h"
#include "ui_novel.h"
#include "ui_player.h"
#include "menu.h"

extern const lv_font_t lv_font_global_16;
extern const lv_font_t lv_font_montserrat_14;
extern const lv_font_t lv_font_montserrat_12;

/* ── 布局 ── */
#define NOVEL_READ_X     2
#define NOVEL_READ_Y     50
#define NOVEL_READ_W     168
#define NOVEL_READ_H     226
#define NOVEL_PAGE_Y     282
#define NOVEL_BTN_W      78
#define NOVEL_BTN_H      32
#define PAGE_BYTES       1024   /* 每页读取字节数 */
#define PAGE_STACK_MAX   256    /* 页首偏移栈容量 (RAM 内, 不落盘) */
#define NOVEL_PAGE_OFFSET 0

/* 电池 (复用音乐页配色) */
#define NBAT_BODY_W      18
#define NBAT_BODY_H      9
#define NBAT_BODY_X      130
#define NBAT_BODY_Y      20
#define NBAT_NUB_W       2
#define NBAT_NUB_H       4
#define NBAT_PAD         2
#define NBAT_FILL_MAX_W  (NBAT_BODY_W - 2 * NBAT_PAD)
#define VBAT_PCT_MIN     3.3f
#define VBAT_PCT_MAX     4.2f

/* ── 静态状态 ── */
static lv_obj_t   *s_root       = NULL;  /* 全屏根容器 */
static lv_obj_t   *s_pct_lbl    = NULL;  /* 顶部中间: 阅读百分比 */
static lv_obj_t   *s_bat_fill   = NULL;  /* 顶部右侧: 电池填充条 */
static lv_obj_t   *s_text       = NULL;  /* 阅读文本标签 */
static lv_obj_t   *s_read_cont  = NULL;  /* 阅读滚动容器 */
static lv_timer_t *s_bat_timer  = NULL;

static FILE       *s_book       = NULL;  /* 当前小说文件 */
static uint32_t    s_book_size  = 0;     /* 文件总字节数 */
static uint32_t    s_page_start = 0;     /* 当前页起始偏移 */
static uint32_t    s_page_next  = 0;     /* 下一页起始偏移 */
static uint32_t    s_page_stack[PAGE_STACK_MAX];  /* 页首偏移历史栈 */
static int         s_page_sp    = 0;     /* 栈顶指针 */

static char        s_buf[PAGE_BYTES + 8];  /* 页数据缓冲 (含结尾 '\0') */

/* ── UTF-8 工具 ── */
static int utf8_char_len(uint8_t c)
{
    if ((c & 0x80) == 0x00) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

/* 更新阅读百分比 (当前页起始 / 总大小) */
static void novel_update_percent(void)
{
    if (!s_pct_lbl) return;
    float p = 0.0f;
    if (s_book_size > 0) {
        p = (float)s_page_start / (float)s_book_size * 100.0f;
        if (p > 100.0f) p = 100.0f;
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "%.1f%%", (double)p);
    lv_label_set_text(s_pct_lbl, buf);
}

/* 读取并显示 offset 起始的一页 */
static void novel_read_page(uint32_t offset)
{
    if (!s_book || !s_text) return;
    if (offset > s_book_size) offset = s_book_size;

    sd_fs_lock();
    if (fseek(s_book, (long)offset, SEEK_SET) != 0) {
        sd_fs_unlock();
        return;
    }
    size_t n = fread(s_buf, 1, PAGE_BYTES, s_book);
    sd_fs_unlock();

    if (n == 0) {
        /* 文件尾: 显示提示并把进度置 100% */
        lv_label_set_text(s_text, "已读完所有内容");
        s_page_start = s_book_size;
        s_page_next  = s_book_size;
        novel_update_percent();
        return;
    }

    /* 若读满一页, 向前回退到完整 UTF-8 字符边界, 避免截断多字节字符 */
    size_t valid = n;
    if (n == PAGE_BYTES) {
        for (int i = (int)n - 1; i >= 0; i--) {
            uint8_t c = (uint8_t)s_buf[i];
            if ((c & 0xC0) != 0x80) {              /* 非续字节 = 字符首字节 */
                int cl = utf8_char_len(c);
                valid = ((size_t)i + (size_t)cl > n) ? (size_t)i : n;
                break;
            }
        }
    }
    s_buf[valid] = '\0';

    /* 控制字符 (除 换行/制表/回车) 替换为空格; UTF-8 字节 (>=0x80) 原样保留 */
    for (size_t i = 0; i < valid; i++) {
        unsigned char c = (unsigned char)s_buf[i];
        if (c < 0x20 && c != '\n' && c != '\r' && c != '\t') {
            s_buf[i] = ' ';
        }
    }

    lv_label_set_text(s_text, s_buf);
    lv_obj_scroll_to_y(s_read_cont, 0, LV_ANIM_OFF);

    s_page_start = offset;
    s_page_next  = offset + (uint32_t)valid;

    novel_update_percent();
    printf("[NOVEL] page start=%lu next=%lu size=%lu\n",
           (unsigned long)s_page_start, (unsigned long)s_page_next,
           (unsigned long)s_book_size);
}

static void novel_next_page(void)
{
    if (!s_book) return;
    if (s_page_next >= s_book_size) return;   /* 已到末尾 */
    if (s_page_sp < PAGE_STACK_MAX) {
        s_page_stack[s_page_sp++] = s_page_start;   /* 记录当前页首供回退 */
    }
    novel_read_page(s_page_next);
}

static void novel_prev_page(void)
{
    if (!s_book) return;
    if (s_page_sp > 0) {
        uint32_t start = s_page_stack[--s_page_sp];
        novel_read_page(start);
    } else {
        novel_read_page(0);   /* 无历史: 回到开头 */
    }
}

/* ── 电池刷新 (1s) ── */
static void novel_bat_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_bat_fill) return;
    float v = atomic_load_float(&g_vbat);
    int pct = (int)((v - VBAT_PCT_MIN) / (VBAT_PCT_MAX - VBAT_PCT_MIN) * 100.0f);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int w = NBAT_FILL_MAX_W * pct / 100;
    if (pct > 0 && w < 1) w = 1;
    lv_obj_set_width(s_bat_fill, w);
    lv_obj_set_style_bg_color(s_bat_fill,
        pct > 30 ? lv_color_hex(0x2E7D32) :
        pct >= 20 ? lv_color_hex(0xFFA000) : lv_color_hex(0xE53935), 0);
}

/* ── 翻页按钮回调 ── */
static void novel_prev_cb(lv_event_t *e) { (void)e; novel_prev_page(); }
static void novel_next_cb(lv_event_t *e) { (void)e; novel_next_page(); }

/* 建一个底部翻页按钮 */
static lv_obj_t *novel_make_page_btn(int x, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(s_root);
    lv_obj_set_pos(btn, x, NOVEL_PAGE_Y);
    lv_obj_set_size(btn, NOVEL_BTN_W, NOVEL_BTN_H);
    lv_obj_set_style_radius(btn, 6, 0);
    /* 深灰主题: 与播放器底部面板一致 (深底 + 描边 + 浅字) */
    lv_obj_set_style_bg_color(btn, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, COLOR_BORDER, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_global_16, 0);
    lv_obj_set_style_text_color(lbl, COLOR_FG, 0);
    lv_obj_center(lbl);
    return btn;
}

/* 构建小说阅读界面 */
static void novel_build(void)
{
    if (s_root) return;

    s_root = lv_obj_create(lv_scr_act());
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_size(s_root, TFT_HOR_RES, TFT_VER_RES);
    lv_obj_set_style_bg_color(s_root, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_radius(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_root, LV_SCROLLBAR_MODE_OFF);

    /* 左上: 文件浏览器按钮 (与音乐页同位置) */
    lv_obj_t *btn_menu = make_icon_btn(s_root, 5, 5, 40, 40, LV_SYMBOL_LIST);
    lv_obj_t *icon_menu = lv_obj_get_child(btn_menu, 0);
    lv_obj_set_style_text_font(icon_menu, &lv_font_montserrat_18, 0);
    lv_obj_add_event_cb(btn_menu, fs_menu_click_cb, LV_EVENT_CLICKED, NULL);

    /* 中上: 阅读百分比 (原 n/n 位置) */
    s_pct_lbl = lv_label_create(s_root);
    lv_obj_set_pos(s_pct_lbl, 54, 20);
    lv_obj_set_size(s_pct_lbl, 65, 16);
    lv_obj_set_style_text_align(s_pct_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_pct_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_pct_lbl, COLOR_MUTED, 0);
    lv_label_set_text(s_pct_lbl, "0.0%");

    /* 右上: 电池图标 (原蓝牙按钮位置) */
    lv_obj_t *bat_body = lv_obj_create(s_root);
    lv_obj_set_pos(bat_body, NBAT_BODY_X, NBAT_BODY_Y);
    lv_obj_set_size(bat_body, NBAT_BODY_W, NBAT_BODY_H);
    lv_obj_set_style_bg_opa(bat_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(bat_body, 2, 0);
    lv_obj_set_style_border_color(bat_body, COLOR_MUTED, 0);
    lv_obj_set_style_border_width(bat_body, 1, 0);
    lv_obj_set_style_pad_all(bat_body, 0, 0);
    lv_obj_set_style_shadow_width(bat_body, 0, 0);
    lv_obj_clear_flag(bat_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(bat_body, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *bat_nub = lv_obj_create(s_root);
    lv_obj_set_pos(bat_nub, NBAT_BODY_X + NBAT_BODY_W, NBAT_BODY_Y + (NBAT_BODY_H - NBAT_NUB_H) / 2);
    lv_obj_set_size(bat_nub, NBAT_NUB_W, NBAT_NUB_H);
    lv_obj_set_style_bg_color(bat_nub, COLOR_MUTED, 0);
    lv_obj_set_style_bg_opa(bat_nub, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bat_nub, 1, 0);
    lv_obj_set_style_border_width(bat_nub, 0, 0);
    lv_obj_set_style_shadow_width(bat_nub, 0, 0);
    lv_obj_clear_flag(bat_nub, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(bat_nub, LV_SCROLLBAR_MODE_OFF);

    s_bat_fill = lv_obj_create(s_root);
    lv_obj_set_pos(s_bat_fill, NBAT_BODY_X + NBAT_PAD, NBAT_BODY_Y + NBAT_PAD);
    lv_obj_set_size(s_bat_fill, NBAT_FILL_MAX_W, NBAT_BODY_H - 2 * NBAT_PAD);
    lv_obj_set_style_bg_color(s_bat_fill, lv_color_hex(0x2E7D32), 0);
    lv_obj_set_style_bg_opa(s_bat_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_bat_fill, 1, 0);
    lv_obj_set_style_border_width(s_bat_fill, 0, 0);
    lv_obj_set_style_shadow_width(s_bat_fill, 0, 0);
    lv_obj_clear_flag(s_bat_fill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_bat_fill, LV_SCROLLBAR_MODE_OFF);

    /* 中部: 阅读框 */
    s_read_cont = lv_obj_create(s_root);
    lv_obj_set_pos(s_read_cont, NOVEL_READ_X, NOVEL_READ_Y);
    lv_obj_set_size(s_read_cont, NOVEL_READ_W, NOVEL_READ_H);
    lv_obj_set_style_bg_color(s_read_cont, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(s_read_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_read_cont, 4, 0);
    lv_obj_set_style_border_color(s_read_cont, COLOR_BORDER, 0);
    lv_obj_set_style_border_width(s_read_cont, 1, 0);
    lv_obj_set_style_pad_all(s_read_cont, 4, 0);
    lv_obj_set_scrollbar_mode(s_read_cont, LV_SCROLLBAR_MODE_AUTO);

    s_text = lv_label_create(s_read_cont);
    lv_obj_set_width(s_text, NOVEL_READ_W - 10);
    lv_obj_set_style_text_font(s_text, &lv_font_global_16, 0);
    lv_obj_set_style_text_color(s_text, COLOR_FG, 0);
    lv_obj_set_style_text_line_space(s_text, 2, 0);
    lv_label_set_long_mode(s_text, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_text, "请点击左上角选择小说文件");

    /* 底部: 两个翻页按钮 */
    novel_make_page_btn(6, "上一页", novel_prev_cb);
    novel_make_page_btn(88, "下一页", novel_next_cb);

    s_bat_timer = lv_timer_create(novel_bat_timer_cb, 1000, NULL);
    novel_bat_timer_cb(NULL);
}

/* 关闭当前文件并重置页状态 */
static void novel_close_file(void)
{
    if (s_book) {
        sd_fs_lock();
        fclose(s_book);
        sd_fs_unlock();
        s_book = NULL;
    }
    s_book_size  = 0;
    s_page_start = 0;
    s_page_next  = 0;
    s_page_sp    = 0;
}

void ui_novel_show(void)
{
    novel_build();
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_root);
    if (s_bat_timer) lv_timer_resume(s_bat_timer);
    if (s_pct_lbl && !s_book) lv_label_set_text(s_pct_lbl, "0.0%");
}

void ui_novel_destroy(void)
{
    novel_close_file();
    if (s_bat_timer) { lv_timer_del(s_bat_timer); s_bat_timer = NULL; }
    if (s_root) { lv_obj_del(s_root); s_root = NULL; }
    s_pct_lbl = NULL;
    s_bat_fill = NULL;
    s_text = NULL;
    s_read_cont = NULL;
}

void ui_novel_open(const char *group, const char *name)
{
    if (!group || !name) return;
    if (!atomic_load_bool(&g_sd_ready)) return;

    if (!s_root) novel_build();

    char path[512];
    fs_build_real_path(group, name, path, sizeof(path));

    novel_close_file();

    sd_fs_lock();
    s_book = fopen(path, "rb");
    if (s_book) {
        if (fseek(s_book, 0, SEEK_END) == 0) {
            long sz = ftell(s_book);
            s_book_size = (sz > 0) ? (uint32_t)sz : 0;
        }
    }
    sd_fs_unlock();

    if (!s_book) {
        if (s_text) lv_label_set_text(s_text, "无法打开小说文件");
        if (s_pct_lbl) lv_label_set_text(s_pct_lbl, "0.0%");
        printf("[NOVEL] open failed: %s\n", path);
        return;
    }

    printf("[NOVEL] open: %s (%lu bytes)\n", path, (unsigned long)s_book_size);
    novel_read_page(0);
}

void ui_novel_on_sd_remove(void)
{
    novel_close_file();
    if (s_text) lv_label_set_text(s_text, "SD 卡已拔出");
    if (s_pct_lbl) lv_label_set_text(s_pct_lbl, "0.0%");
}

bool ui_novel_is_active(void)
{
    return s_root != NULL;
}
