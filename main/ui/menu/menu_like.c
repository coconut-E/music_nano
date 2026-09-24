#include <string.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "ui_core.h"
#include "ui_player.h"
#include "song_hash.h"
#include "likes.h"
#include "menu.h"
#include "menu_like.h"

/* 面板尺寸/位置 (屏幕 172x320, 贴顶弹出) */
#define PANEL_W       168
#define PANEL_H       100
#define PANEL_X       1
#define PANEL_Y       8
#define PANEL_RADIUS  10

/* 面板内部布局 (面板局部坐标) */
#define LBL_W         84
#define LBL_H         20
#define LBL_Y         10
#define BTN_W         72
#define BTN_H         45
#define BTN_Y         42
#define COL_L_CX      (PANEL_W / 5)      
#define COL_R_CX      (PANEL_W * 4 / 6)  

#define COLOR_LIKE_RED   lv_color_hex(0xE53935)   /* 已喜欢 / 删除按钮 */

/* 顶部滑入动画参数 */
#define ANIM_MS_OPEN   150
#define ANIM_MS_CLOSE  100
#define ANIM_TICK_MS   16
#define SCR_BYTES      (TFT_HOR_RES * TFT_VER_RES * 2)
#define PANEL_BYTES    (PANEL_W * PANEL_H * 2)

extern const lv_font_t lv_font_global_16;
extern const lv_font_t lv_font_heart_44;

static lv_obj_t *s_overlay    = NULL;   /* 全屏透明遮罩 (点背景关闭) */
static lv_obj_t *s_panel      = NULL;   /* 深色面板 */
static lv_obj_t *s_heart_lbl  = NULL;   /* 心形符号标签 */
static lv_obj_t *s_del_lbl    = NULL;   /* 删除按钮文字 */

static bool     s_open        = false;
static bool     s_liked       = false;
static bool     s_del_confirm = false;
static uint32_t s_hash        = 0;

/* ── 自包含滑入动画状态 (抄自 menu_anim.c 的截图合成法) ── */
static lv_obj_t   *s_anim_bg_img    = NULL;   /* 顶层: 主界面截图 */
static lv_obj_t   *s_anim_panel_img = NULL;   /* 顶层: 面板合成图 */
static lv_timer_t *s_anim_timer     = NULL;
static uint8_t    *s_anim_buf_main  = NULL;   /* PSRAM: 主界面截图 */
static uint8_t    *s_anim_buf_panel = NULL;   /* PSRAM: 面板截图 */
static uint8_t    *s_anim_buf_comp  = NULL;   /* PSRAM: 面板合成缓冲 */
static lv_img_dsc_t s_dsc_main;
static lv_img_dsc_t s_dsc_panel;
static lv_img_dsc_t s_dsc_comp;
static int64_t     s_anim_t0        = 0;
static bool        s_anim_open      = true;
static int         s_anim_prev_y    = 0;
static bool        s_animating      = false;

static void like_anim_timer_cb(lv_timer_t *tmr);

/* PSRAM 优先分配, 失败回退内部 RAM */
static void *like_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_malloc(n, MALLOC_CAP_8BIT);
    return p;
}

static void like_anim_free_buffers(void)
{
    if (s_anim_buf_main)  { heap_caps_free(s_anim_buf_main);  s_anim_buf_main  = NULL; }
    if (s_anim_buf_panel) { heap_caps_free(s_anim_buf_panel); s_anim_buf_panel = NULL; }
    if (s_anim_buf_comp)  { heap_caps_free(s_anim_buf_comp);  s_anim_buf_comp  = NULL; }
}

static void like_anim_free_objs(void)
{
    if (s_anim_bg_img)    { lv_obj_del(s_anim_bg_img);    s_anim_bg_img    = NULL; }
    if (s_anim_panel_img) { lv_obj_del(s_anim_panel_img); s_anim_panel_img = NULL; }
}

/* ─────────────────────── 控件创建/销毁 ─────────────────────── */

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, int cx, lv_color_t color)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_set_pos(lbl, cx - LBL_W / 2, LBL_Y);
    lv_obj_set_size(lbl, LBL_W, LBL_H);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_global_16, 0);
    lv_obj_set_style_text_color(lbl, color, 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_label_set_text(lbl, text);
    return lbl;
}

/* 遮罩点击 → 关闭 */
static void overlay_click_cb(lv_event_t *e)
{
    (void)e;
    like_menu_close();
}

/* 心形按钮: 切换喜欢状态并落盘 */
static void heart_click_cb(lv_event_t *e)
{
    (void)e;
    s_liked = !s_liked;
    if (s_liked) {
        likes_add(s_hash);
    } else {
        likes_remove(s_hash);
    }
    if (s_heart_lbl) {
        lv_obj_set_style_text_color(s_heart_lbl,
                                    s_liked ? COLOR_LIKE_RED : COLOR_MUTED, 0);
    }
    fs_browser_likes_changed();   /* 让文件浏览器重建, 刷新爱心图标 */
}

/* 删除按钮: 第一次变"确认", 再按关闭弹框并执行删除 */
static void del_click_cb(lv_event_t *e)
{
    (void)e;
    if (!s_del_confirm) {
        s_del_confirm = true;
        if (s_del_lbl) lv_label_set_text(s_del_lbl, "确认");
    } else {
        like_menu_close();
        player_request_delete_current_file();   /* 异步: 等音频停稳后删除并重扫 */
    }
}

/* 创建遮罩 + 面板 (动画 open_real 等价物) */
static void like_panel_create(void)
{
    s_overlay = lv_btn_create(lv_scr_act());
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_size(s_overlay, TFT_HOR_RES, TFT_VER_RES);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_shadow_width(s_overlay, 0, 0);
    /* 清掉按钮主题默认内边距 (否则子面板会被挤偏 PAD_DEF/PAD_SMALL 像素) */
    lv_obj_set_style_pad_all(s_overlay, 0, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_overlay, overlay_click_cb, LV_EVENT_CLICKED, NULL);

    s_panel = lv_obj_create(s_overlay);
    lv_obj_set_pos(s_panel, PANEL_X, PANEL_Y);
    lv_obj_set_size(s_panel, PANEL_W, PANEL_H);
    lv_obj_set_style_bg_color(s_panel, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_panel, PANEL_RADIUS, 0);
    lv_obj_set_style_border_color(s_panel, COLOR_BORDER, 0);
    lv_obj_set_style_border_width(s_panel, 1, 0);
    lv_obj_set_style_shadow_width(s_panel, 0, 0);
    lv_obj_set_style_pad_all(s_panel, 0, 0);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);

    /* 顶部标签: 喜欢 / 删除该音乐 */
    make_label(s_panel, "喜欢", COL_L_CX, COLOR_FG);
    make_label(s_panel, "删除该音乐", COL_R_CX, COLOR_FG);

    /* 心形按钮 (44px 符号 ♥) */
    lv_obj_t *heart_btn = lv_btn_create(s_panel);
    lv_obj_set_pos(heart_btn, COL_L_CX - BTN_W / 2, BTN_Y);
    lv_obj_set_size(heart_btn, BTN_W, BTN_H);
    lv_obj_set_style_bg_opa(heart_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(heart_btn, 0, 0);
    lv_obj_set_style_shadow_width(heart_btn, 0, 0);
    lv_obj_add_event_cb(heart_btn, heart_click_cb, LV_EVENT_CLICKED, NULL);

    s_heart_lbl = lv_label_create(heart_btn);
    lv_label_set_text(s_heart_lbl, "♥");
    lv_obj_set_style_text_font(s_heart_lbl, &lv_font_heart_44, 0);
    lv_obj_set_style_text_color(s_heart_lbl,
                                s_liked ? COLOR_LIKE_RED : COLOR_MUTED, 0);
    lv_obj_center(s_heart_lbl);

    /* 删除按钮 (红) */
    lv_obj_t *del_btn = lv_btn_create(s_panel);
    lv_obj_set_pos(del_btn, COL_R_CX - BTN_W / 2, BTN_Y);
    lv_obj_set_size(del_btn, BTN_W, BTN_H);
    lv_obj_set_style_bg_color(del_btn, COLOR_LIKE_RED, 0);
    lv_obj_set_style_bg_opa(del_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(del_btn, 8, 0);
    lv_obj_set_style_border_width(del_btn, 0, 0);
    lv_obj_set_style_shadow_width(del_btn, 0, 0);
    lv_obj_add_event_cb(del_btn, del_click_cb, LV_EVENT_CLICKED, NULL);

    s_del_lbl = lv_label_create(del_btn);
    lv_label_set_text(s_del_lbl, "删除");
    lv_obj_set_style_text_font(s_del_lbl, &lv_font_global_16, 0);
    lv_obj_set_style_text_color(s_del_lbl, lv_color_white(), 0);
    lv_obj_center(s_del_lbl);

    s_open = true;
}

/* 销毁遮罩 + 面板 (动画 close_real 等价物) */
static void like_panel_destroy(void)
{
    if (s_overlay) lv_obj_del(s_overlay);   /* 连带删除全部子控件 */
    s_overlay    = NULL;
    s_panel      = NULL;
    s_heart_lbl  = NULL;
    s_del_lbl    = NULL;
    s_del_confirm = false;
    s_open       = false;
}

/* ─────────────────────── 滑入动画 ─────────────────────── */

/* 面板截图按圆角叠加到主界面背景上 (整尺寸一次性合成) */
static void like_anim_compose(void)
{
    const int pw = PANEL_W, ph = PANEL_H, r = PANEL_RADIUS;

    for (int y = 0; y < ph; y++) {
        uint8_t *dst = s_anim_buf_comp + y * pw * 2;
        uint8_t *bg  = s_anim_buf_main + ((PANEL_Y + y) * TFT_HOR_RES + PANEL_X) * 2;
        uint8_t *fs  = s_anim_buf_panel + y * pw * 2;

        /* 该行圆角内 x 区间 [rx0, rx1] */
        int rx0 = 0, rx1 = pw - 1;
        if (y < r) {
            int dy = r - y;
            if (dy > 0) {
                lv_sqrt_res_t q;
                lv_sqrt(r * r - dy * dy, &q, 0x8000);
                int s = q.i;
                rx0 = r - s; rx1 = pw - 1 - (r - s);
            }
        } else if (y >= ph - r) {
            int dy = y - (ph - 1 - r);
            if (dy > 0) {
                lv_sqrt_res_t q;
                lv_sqrt(r * r - dy * dy, &q, 0x8000);
                int s = q.i;
                rx0 = r - s; rx1 = pw - 1 - (r - s);
            }
        }
        if (rx0 < 0) rx0 = 0;
        if (rx1 > pw - 1) rx1 = pw - 1;
        if (rx0 > rx1) rx0 = rx1;

        /* 圆角内取面板, 圆角外取背景 */
        int pos = 0;
        while (pos < pw) {
            if (pos < rx0) {
                memcpy(dst + pos * 2, bg + pos * 2, (rx0 - pos) * 2);
                pos = rx0;
            } else if (pos <= rx1) {
                memcpy(dst + pos * 2, fs + pos * 2, (rx1 - pos + 1) * 2);
                pos = rx1 + 1;
            } else {
                memcpy(dst + pos * 2, bg + pos * 2, (pw - pos) * 2);
                pos = pw;
            }
        }
    }
}

/* 建立顶层图片并启动动画定时器 (缓冲/截图须已就绪) */
static void like_anim_show(bool open)
{
    lv_obj_add_flag(lv_scr_act(), LV_OBJ_FLAG_HIDDEN);

    s_anim_bg_img = lv_img_create(lv_layer_top());
    lv_img_set_src(s_anim_bg_img, &s_dsc_main);
    lv_obj_set_pos(s_anim_bg_img, 0, 0);

    memset(&s_dsc_comp, 0, sizeof(s_dsc_comp));
    s_dsc_comp.header.w = PANEL_W;
    s_dsc_comp.header.h = PANEL_H;
    s_dsc_comp.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_dsc_comp.data_size = PANEL_BYTES;
    s_dsc_comp.data = s_anim_buf_comp;

    s_anim_panel_img = lv_img_create(lv_layer_top());
    lv_img_set_src(s_anim_panel_img, &s_dsc_comp);

    s_anim_open   = open;
    s_anim_prev_y = open ? (PANEL_Y - PANEL_H) : PANEL_Y;
    lv_obj_set_pos(s_anim_panel_img, PANEL_X, s_anim_prev_y);

    s_anim_t0   = esp_timer_get_time();
    s_animating = true;
    s_anim_timer = lv_timer_create(like_anim_timer_cb, ANIM_TICK_MS, NULL);
    lv_timer_ready(s_anim_timer);
}

/* 回弹缓动 (easeOutBack): 先快后慢 + 过冲, p 会短暂超过 1 再回落到 1 */
static float ease_out_back(float t)
{
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.0f;
    float u = t - 1.0f;
    return 1.0f + c3 * u * u * u + c1 * u * u;
}

static void like_anim_timer_cb(lv_timer_t *tmr)
{
    int64_t dur = s_anim_open ? ANIM_MS_OPEN : ANIM_MS_CLOSE;
    int64_t el = (esp_timer_get_time() - s_anim_t0) / 1000;   /* ms */
    if (el < 0) el = 0;
    if (el > dur) el = dur;

    int y;
    if (s_anim_open) {
        /* 打开: 从 PANEL_Y-PANEL_H 滑到 PANEL_Y, 先快后慢并过冲一下 */
        float p = ease_out_back((float)el / (float)dur);
        y = (PANEL_Y - PANEL_H) + (int)(PANEL_H * p + 0.5f);
    } else {
        /* 关闭: 线性回缩到屏幕上方 */
        y = PANEL_Y - (int)((int64_t)PANEL_H * el / dur);
    }
    lv_obj_set_y(s_anim_panel_img, y);

    /* 失效旧/新面板矩形并集 */
    int y0 = (y < s_anim_prev_y) ? y : s_anim_prev_y;
    int y1 = ((y > s_anim_prev_y) ? y : s_anim_prev_y) + PANEL_H - 1;
    if (y0 < 0) y0 = 0;
    if (y1 > TFT_VER_RES - 1) y1 = TFT_VER_RES - 1;
    lv_area_t a;
    a.x1 = PANEL_X;
    a.x2 = PANEL_X + PANEL_W - 1;
    a.y1 = y0;
    a.y2 = y1;
    lv_obj_invalidate_area(s_anim_bg_img, &a);
    s_anim_prev_y = y;

    lv_refr_now(lv_disp_get_default());

    if (el >= dur) {
        lv_timer_del(tmr);
        s_anim_timer = NULL;
        like_anim_free_objs();
        like_anim_free_buffers();
        if (!s_anim_open) like_panel_destroy();
        lv_obj_clear_flag(lv_scr_act(), LV_OBJ_FLAG_HIDDEN);
        s_animating = false;
    }
}

/* ─────────────────────── 对外接口 ─────────────────────── */

void like_menu_open(void)
{
    if (s_open || s_animating) return;

    /* 按文件名 (去扩展名) 取键: 与文件浏览器一致 */
    const char *name = player_current_name();
    if (!name) return;
    char key[160];
    song_hash_name_key(name, key, sizeof(key));
    s_hash = song_hash32(key, strlen(key));

    likes_init();
    s_liked       = likes_contains(s_hash);
    s_del_confirm = false;

    /* 1. 主界面截图 (面板未创建) */
    s_anim_buf_main = like_alloc(SCR_BYTES);
    if (!s_anim_buf_main ||
        lv_snapshot_take_to_buf(lv_scr_act(), LV_IMG_CF_TRUE_COLOR,
                                &s_dsc_main, s_anim_buf_main, SCR_BYTES) != LV_RES_OK) {
        /* 截图失败: 直接创建面板, 无动画 */
        like_anim_free_buffers();
        like_panel_create();
        return;
    }

    /* 2. 建真实面板 + 3. 面板截图 + 合成 */
    like_panel_create();
    s_anim_buf_panel = like_alloc(PANEL_BYTES);
    s_anim_buf_comp  = like_alloc(PANEL_BYTES);
    if (!s_anim_buf_panel || !s_anim_buf_comp ||
        lv_snapshot_take_to_buf(s_panel, LV_IMG_CF_TRUE_COLOR,
                                &s_dsc_panel, s_anim_buf_panel, PANEL_BYTES) != LV_RES_OK) {
        /* 面板截图失败: 面板已可见, 释放缓冲即可 */
        like_anim_free_buffers();
        return;
    }

    like_anim_compose();
    like_anim_show(true);
}

void like_menu_close(void)
{
    if (!s_open || s_animating) return;

    /* 1. 面板截图 */
    s_anim_buf_panel = like_alloc(PANEL_BYTES);
    s_anim_buf_comp  = like_alloc(PANEL_BYTES);
    s_anim_buf_main  = like_alloc(SCR_BYTES);
    if (s_anim_buf_panel && s_anim_buf_comp && s_anim_buf_main &&
        lv_snapshot_take_to_buf(s_panel, LV_IMG_CF_TRUE_COLOR,
                                &s_dsc_panel, s_anim_buf_panel, PANEL_BYTES) == LV_RES_OK) {
        /* 2. 临时隐藏遮罩, 截干净的主界面 */
        lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
        bool ok = (lv_snapshot_take_to_buf(lv_scr_act(), LV_IMG_CF_TRUE_COLOR,
                                           &s_dsc_main, s_anim_buf_main, SCR_BYTES) == LV_RES_OK);
        lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);

        if (ok) {
            like_anim_compose();
            like_anim_show(false);
            return;
        }
    }

    /* 回退: 直接关闭 */
    like_anim_free_buffers();
    like_panel_destroy();
}

bool like_menu_is_open(void)
{
    return s_open;
}
