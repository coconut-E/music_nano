#include <string.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "ui_core.h"
#include "ui_player.h"
#include "settings.h"
#include "menu_mode.h"

/* 面板尺寸/位置 (屏幕 172x320, 贴底弹出) */
#define PANEL_W       168
#define PANEL_H       120
#define PANEL_X       1
#define PANEL_Y       (TFT_VER_RES - PANEL_H - 8)   /* 192 */
#define PANEL_RADIUS  10

/* 步进器布局 (面板局部坐标) */
#define BTN_SIZE      40
#define BTN_Y         62
#define MINUS_X       20
#define NUM_X         64
#define PLUS_X        108

/* 底部滑入动画参数 */
#define ANIM_MS_OPEN   120
#define ANIM_MS_CLOSE  90
#define ANIM_TICK_MS   16
#define SCR_BYTES      (TFT_HOR_RES * TFT_VER_RES * 2)
#define PANEL_BYTES    (PANEL_W * PANEL_H * 2)

extern const lv_font_t lv_font_global_16;

static lv_obj_t *s_overlay   = NULL;   /* 全屏透明遮罩 (点背景关闭) */
static lv_obj_t *s_panel     = NULL;   /* 深色面板 */
static lv_obj_t *s_num_lbl   = NULL;   /* 数字标签 */
static lv_obj_t *s_minus_btn = NULL;   /* － 按钮 */
static lv_obj_t *s_plus_btn  = NULL;   /* ＋ 按钮 */

static bool s_open      = false;
static bool s_animating = false;

/* ── 自包含滑入动画状态 (抄自 menu_like.c 的截图合成法, 方向改为底部向上) ── */
static lv_obj_t   *s_anim_bg_img    = NULL;
static lv_obj_t   *s_anim_panel_img = NULL;
static lv_timer_t *s_anim_timer     = NULL;
static uint8_t    *s_anim_buf_main  = NULL;
static uint8_t    *s_anim_buf_panel = NULL;
static uint8_t    *s_anim_buf_comp  = NULL;
static lv_img_dsc_t s_dsc_main;
static lv_img_dsc_t s_dsc_panel;
static lv_img_dsc_t s_dsc_comp;
static int64_t     s_anim_t0     = 0;
static bool        s_anim_open   = true;
static int         s_anim_prev_y = 0;

static void mode_anim_timer_cb(lv_timer_t *tmr);
static void mode_menu_close_internal(void);

/* PSRAM 优先分配, 失败回退内部 RAM */
static void *mode_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_malloc(n, MALLOC_CAP_8BIT);
    return p;
}

static void mode_anim_free_buffers(void)
{
    if (s_anim_buf_main)  { heap_caps_free(s_anim_buf_main);  s_anim_buf_main  = NULL; }
    if (s_anim_buf_panel) { heap_caps_free(s_anim_buf_panel); s_anim_buf_panel = NULL; }
    if (s_anim_buf_comp)  { heap_caps_free(s_anim_buf_comp);  s_anim_buf_comp  = NULL; }
}

static void mode_anim_free_objs(void)
{
    if (s_anim_bg_img)    { lv_obj_del(s_anim_bg_img);    s_anim_bg_img    = NULL; }
    if (s_anim_panel_img) { lv_obj_del(s_anim_panel_img); s_anim_panel_img = NULL; }
}

/* ─────────────────────── 控件创建/销毁 ─────────────────────── */

/* 数字/按钮状态同步: 显示当前值, 边界禁用对应按钮 */
static void mode_update_num(void)
{
    int v = player_get_loop_count();

    if (s_num_lbl) lv_label_set_text_fmt(s_num_lbl, "%d", v);

    if (s_minus_btn) {
        if (v <= LOOP_COUNT_MIN) lv_obj_add_state(s_minus_btn, LV_STATE_DISABLED);
        else                     lv_obj_clear_state(s_minus_btn, LV_STATE_DISABLED);
    }
    if (s_plus_btn) {
        if (v >= LOOP_COUNT_MAX) lv_obj_add_state(s_plus_btn, LV_STATE_DISABLED);
        else                     lv_obj_clear_state(s_plus_btn, LV_STATE_DISABLED);
    }
}

static void mode_minus_cb(lv_event_t *e)
{
    (void)e;
    int v = player_get_loop_count();
    if (v > LOOP_COUNT_MIN) {
        player_set_loop_count(v - 1);
        mode_update_num();
    }
}

static void mode_plus_cb(lv_event_t *e)
{
    (void)e;
    int v = player_get_loop_count();
    if (v < LOOP_COUNT_MAX) {
        player_set_loop_count(v + 1);
        mode_update_num();
    }
}

static void overlay_click_cb(lv_event_t *e)
{
    (void)e;
    mode_menu_close_internal();
}

/* 建一个圆形深色符号按钮 */
static lv_obj_t *make_step_btn(lv_obj_t *parent, int x, const char *symbol,
                               lv_event_cb_t cb, lv_obj_t **out)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_pos(btn, x, BTN_Y);
    lv_obj_set_size(btn, BTN_SIZE, BTN_SIZE);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2A2A2A), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, symbol);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl, COLOR_FG, 0);
    lv_obj_center(lbl);

    if (out) *out = btn;
    return btn;
}

/* 创建遮罩 + 面板 (动画 open_real 等价物) */
static void mode_panel_create(void)
{
    s_overlay = lv_btn_create(lv_scr_act());
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_size(s_overlay, TFT_HOR_RES, TFT_VER_RES);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_shadow_width(s_overlay, 0, 0);
    /* 清掉按钮主题默认内边距, 否则子面板会被挤偏 */
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

    /* 文字标签 (较长, 自动折行) */
    lv_obj_t *lbl = lv_label_create(s_panel);
    lv_obj_set_pos(lbl, 8, 8);
    lv_obj_set_size(lbl, PANEL_W - 16, 44);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_global_16, 0);
    lv_obj_set_style_text_color(lbl, COLOR_FG, 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_label_set_text(lbl, "随机/顺序到喜欢的\n单曲循环次数");

    /* － 数字 ＋ 步进器 */
    make_step_btn(s_panel, MINUS_X, LV_SYMBOL_MINUS, mode_minus_cb, &s_minus_btn);

    s_num_lbl = lv_label_create(s_panel);
    lv_obj_set_pos(s_num_lbl, NUM_X, BTN_Y+8);
    lv_obj_set_size(s_num_lbl, BTN_SIZE, BTN_SIZE);
    lv_obj_set_style_text_align(s_num_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_num_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_num_lbl, COLOR_ACCENT, 0);
    lv_label_set_text(s_num_lbl, "1");

    make_step_btn(s_panel, PLUS_X, LV_SYMBOL_PLUS, mode_plus_cb, &s_plus_btn);

    mode_update_num();
    s_open = true;
}

/* 销毁遮罩 + 面板 (动画 close_real 等价物) */
static void mode_panel_destroy(void)
{
    if (s_overlay) lv_obj_del(s_overlay);
    s_overlay   = NULL;
    s_panel     = NULL;
    s_num_lbl   = NULL;
    s_minus_btn = NULL;
    s_plus_btn  = NULL;
    s_open      = false;
}

/* ─────────────────────── 滑入动画 ─────────────────────── */

/* 面板截图按圆角叠加到主界面背景上 (整尺寸一次性合成) */
static void mode_anim_compose(void)
{
    const int pw = PANEL_W, ph = PANEL_H, r = PANEL_RADIUS;

    for (int y = 0; y < ph; y++) {
        uint8_t *dst = s_anim_buf_comp + y * pw * 2;
        uint8_t *bg  = s_anim_buf_main + ((PANEL_Y + y) * TFT_HOR_RES + PANEL_X) * 2;
        uint8_t *fs  = s_anim_buf_panel + y * pw * 2;

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

/* 建立顶层图片并启动动画定时器 */
static void mode_anim_show(bool open)
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
    s_anim_prev_y = open ? TFT_VER_RES : PANEL_Y;
    lv_obj_set_pos(s_anim_panel_img, PANEL_X, s_anim_prev_y);

    s_anim_t0   = esp_timer_get_time();
    s_animating = true;
    s_anim_timer = lv_timer_create(mode_anim_timer_cb, ANIM_TICK_MS, NULL);
    lv_timer_ready(s_anim_timer);
}

/* 回弹缓动 (easeOutBack): 先快后慢 + 过冲 */
static float ease_out_back(float t)
{
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.0f;
    float u = t - 1.0f;
    return 1.0f + c3 * u * u * u + c1 * u * u;
}

static void mode_anim_timer_cb(lv_timer_t *tmr)
{
    int64_t dur = s_anim_open ? ANIM_MS_OPEN : ANIM_MS_CLOSE;
    int64_t el = (esp_timer_get_time() - s_anim_t0) / 1000;   /* ms */
    if (el < 0) el = 0;
    if (el > dur) el = dur;

    const int span = TFT_VER_RES - PANEL_Y;   /* 128 */
    int y;
    if (s_anim_open) {
        /* 打开: 从屏底滑到 PANEL_Y, 先快后慢并过冲 */
        float p = ease_out_back((float)el / (float)dur);
        y = PANEL_Y + (int)((float)span * (1.0f - p) + 0.5f);
    } else {
        /* 关闭: 线性滑回屏底 */
        y = PANEL_Y + (int)((int64_t)span * el / dur);
    }
    lv_obj_set_y(s_anim_panel_img, y);

    /* 失效旧/新面板矩形并集 */
    int y0 = (y < s_anim_prev_y) ? y : s_anim_prev_y;
    int y1 = ((y > s_anim_prev_y) ? y : s_anim_prev_y) + PANEL_H - 1;
    if (y0 < 0) y0 = 0;
    if (y1 > TFT_VER_RES - 1) y1 = TFT_VER_RES - 1;
    if (y0 <= y1) {
        lv_area_t a;
        a.x1 = PANEL_X;
        a.x2 = PANEL_X + PANEL_W - 1;
        a.y1 = y0;
        a.y2 = y1;
        lv_obj_invalidate_area(s_anim_bg_img, &a);
    }
    s_anim_prev_y = y;

    lv_refr_now(lv_disp_get_default());

    if (el >= dur) {
        lv_timer_del(tmr);
        s_anim_timer = NULL;
        mode_anim_free_objs();
        mode_anim_free_buffers();
        if (!s_anim_open) mode_panel_destroy();
        lv_obj_clear_flag(lv_scr_act(), LV_OBJ_FLAG_HIDDEN);
        s_animating = false;
    }
}

/* ─────────────────────── 对外接口 ─────────────────────── */

void mode_menu_open(void)
{
    if (s_open || s_animating) return;

    /* 1. 主界面截图 (面板未创建) */
    s_anim_buf_main = mode_alloc(SCR_BYTES);
    if (!s_anim_buf_main ||
        lv_snapshot_take_to_buf(lv_scr_act(), LV_IMG_CF_TRUE_COLOR,
                                &s_dsc_main, s_anim_buf_main, SCR_BYTES) != LV_RES_OK) {
        mode_anim_free_buffers();
        mode_panel_create();
        return;
    }

    /* 2. 建真实面板 + 3. 面板截图 + 合成 */
    mode_panel_create();
    s_anim_buf_panel = mode_alloc(PANEL_BYTES);
    s_anim_buf_comp  = mode_alloc(PANEL_BYTES);
    if (!s_anim_buf_panel || !s_anim_buf_comp ||
        lv_snapshot_take_to_buf(s_panel, LV_IMG_CF_TRUE_COLOR,
                                &s_dsc_panel, s_anim_buf_panel, PANEL_BYTES) != LV_RES_OK) {
        mode_anim_free_buffers();
        return;
    }

    mode_anim_compose();
    mode_anim_show(true);
}

static void mode_menu_close_internal(void)
{
    if (!s_open || s_animating) return;

    s_anim_buf_panel = mode_alloc(PANEL_BYTES);
    s_anim_buf_comp  = mode_alloc(PANEL_BYTES);
    s_anim_buf_main  = mode_alloc(SCR_BYTES);
    if (s_anim_buf_panel && s_anim_buf_comp && s_anim_buf_main &&
        lv_snapshot_take_to_buf(s_panel, LV_IMG_CF_TRUE_COLOR,
                                &s_dsc_panel, s_anim_buf_panel, PANEL_BYTES) == LV_RES_OK) {
        lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
        bool ok = (lv_snapshot_take_to_buf(lv_scr_act(), LV_IMG_CF_TRUE_COLOR,
                                           &s_dsc_main, s_anim_buf_main, SCR_BYTES) == LV_RES_OK);
        lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);

        if (ok) {
            mode_anim_compose();
            mode_anim_show(false);
            return;
        }
    }

    mode_anim_free_buffers();
    mode_panel_destroy();
}
