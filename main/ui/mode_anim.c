#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "lvgl.h"
#include "ui_core.h"
#include "mode_anim.h"

/* ────────────────────────────────────────────
 * 模式切换"光圈缩放"过渡动画 (参考 code.html 的 iris, 合成方式仿 menu_anim.c)
 *
 * 原理: 快照旧界面到 PSRAM, 隐藏真实控件, 在 LVGL 顶层放一张全屏图片;
 * 定时器逐帧把"缩放+圆角+透明度"作用到快照上写入合成缓冲, 手动刷新;
 * 全黑时销毁/重建 UI 并快照新界面, 再反向展开, 最后恢复真实控件。
 *
 * 视觉端点 (对应 CSS keyframes):
 *   scale  1.0 ↔ 0.5      radius 0 ↔ 80px      opacity 1 ↔ 0
 *   退出 ease-in, 进入 ease-out; 黑底由合成图的黑色提供 (背光保持常亮)。
 * ──────────────────────────────────────────── */

static const char *TAG = "MODE_ANIM";

#define MA_W        TFT_HOR_RES         /* 172 */
#define MA_H        TFT_VER_RES         /* 320 */
#define MA_BYTES    (MA_W * MA_H * 2)   /* RGB565 全屏字节数 */

#define MA_CLOSE_MS 150                 /* 退出动画时长 */
#define MA_OPEN_MS  180                 /* 进入动画时长 */
#define MA_TICK_MS  16                  /* 动画帧间隔 */

#define MA_SCALE_MIN (65536 / 2)        /* 最小缩放 0.5 (Q16.16) */
#define MA_SCALE_MAX (65536)            /* 最大缩放 1.0 (Q16.16) */
#define MA_RADIUS_MAX 80                /* 最大圆角半径 (元素局部 px) */

typedef enum { MA_PHASE_IDLE = 0, MA_PHASE_CLOSE, MA_PHASE_OPEN } ma_phase_t;

static uint8_t     *s_src   = NULL;    /* 源快照 (旧界面 → 新界面) */
static uint8_t     *s_comp  = NULL;    /* 合成缓冲 (顶层图显示) */
static lv_img_dsc_t s_dsc;             /* 合成缓冲描述符 */
static lv_obj_t    *s_img   = NULL;    /* 顶层全屏图 */
static lv_timer_t  *s_timer = NULL;    /* 动画驱动定时器 */
static ma_phase_t   s_phase = MA_PHASE_IDLE;
static int64_t      s_t0    = 0;       /* 本阶段起始时刻 (us) */
static void       (*s_work)(void) = NULL;   /* 黑场切换工作 */
static void       (*s_done)(void) = NULL;   /* 全部结束回调 */
static bool         s_work_done = false;

/* PSRAM 优先分配, 失败回退内部 RAM */
static void *ma_alloc(uint32_t bytes)
{
    void *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!p) p = heap_caps_malloc(bytes, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    return p;
}

/* 单像素按 alpha 混合到黑 (alpha=0..255, 实际系数 f=alpha+1 取值 1..256) */
static inline uint16_t ma_blend_black(uint16_t px, int f)
{
    lv_color16_t c;
    c.full = px;
    uint16_t g = (uint16_t)(((uint16_t)c.ch.green_h << 3) | c.ch.green_l);
    c.ch.red   = (uint16_t)(((uint16_t)c.ch.red * f) >> 8);
    c.ch.blue  = (uint16_t)(((uint16_t)c.ch.blue * f) >> 8);
    g          = (uint16_t)((g * f) >> 8);
    c.ch.green_h = (uint16_t)(g >> 3);
    c.ch.green_l = (uint16_t)(g & 0x7);
    return c.full;
}

/* 单帧合成: 把源快照按 scale/radius/alpha 画到合成缓冲.
 * s_q16=缩放(Q16.16), radius=圆角(元素局部px), alpha=透明度(0..255).
 * 目标像素在"缩放后的圆角矩形"内取源像素(可乘 alpha), 否则填黑。 */
static void ma_compose(int s_q16, int radius, int alpha)
{
    uint16_t *src = (uint16_t *)s_src;
    uint16_t *dst = (uint16_t *)s_comp;
    int ws = (MA_W * s_q16) >> 16;          /* 缩放后宽 */
    int hs = (MA_H * s_q16) >> 16;          /* 缩放后高 */
    if (ws < 1) ws = 1;
    if (hs < 1) hs = 1;
    int x0 = (MA_W - ws) >> 1;              /* 居中 */
    int y0 = (MA_H - hs) >> 1;
    int f = alpha + 1;                      /* 1..256 */
    bool opaque = (alpha >= 255);

    /* 预计算"目标列 → 源列"映射 (每帧 172 次除法, 避免逐像素除) */
    static int xmap[MA_W];
    for (int dx = x0; dx < x0 + ws && dx < MA_W; dx++) {
        int sx = (int)(((int64_t)(dx - x0) * MA_W) / ws);
        if (sx < 0) sx = 0;
        else if (sx >= MA_W) sx = MA_W - 1;
        xmap[dx] = sx;
    }

    for (int dy = 0; dy < MA_H; dy++) {
        uint16_t *drow = dst + (size_t)dy * MA_W;

        /* 缩放矩形之外: 整行黑 */
        if (dy < y0 || dy >= y0 + hs) {
            memset(drow, 0, MA_W * 2);
            continue;
        }

        int ey = (int)(((int64_t)(dy - y0) * MA_H) / hs);   /* 元素局部 y */
        if (ey < 0) ey = 0;
        else if (ey >= MA_H) ey = MA_H - 1;

        /* 该元素行的圆角内 x 区间 [elo, ehi) (元素局部坐标) */
        int elo = 0, ehi = MA_W;
        if (radius > 0) {
            int dyc = -1;   /* 距圆心纵向距离 */
            if (ey < radius)                       dyc = radius - ey;
            else if (ey >= MA_H - radius)          dyc = ey - (MA_H - 1 - radius);
            if (dyc > 0) {
                int v = radius * radius - dyc * dyc;
                if (v > 0) {
                    lv_sqrt_res_t q;
                    lv_sqrt((uint32_t)v, &q, 0x8000);
                    int inset = radius - q.i;
                    elo = inset;
                    ehi = MA_W - inset;
                }
            }
        }

        /* 映射到目标缓冲 x 区间 (取整后可能差 1 像素, 无碍) */
        int dlo = x0 + (int)(((int64_t)elo * ws) / MA_W);
        int dhi = x0 + (int)(((int64_t)ehi * ws) / MA_W);
        if (dlo < 0) dlo = 0;
        if (dhi > MA_W) dhi = MA_W;
        if (dlo > dhi) dlo = dhi;

        if (dlo > 0) memset(drow, 0, (size_t)dlo * 2);

        const uint16_t *srow = src + (size_t)ey * MA_W;
        if (opaque) {
            for (int dx = dlo; dx < dhi; dx++)
                drow[dx] = srow[xmap[dx]];
        } else {
            for (int dx = dlo; dx < dhi; dx++)
                drow[dx] = ma_blend_black(srow[xmap[dx]], f);
        }

        if (dhi < MA_W) memset(drow + dhi, 0, (size_t)(MA_W - dhi) * 2);
    }
}

/* 进度缓动: v/dur (ms) → 0..256. open=false 用 ease-in(二次), true 用 ease-out(二次) */
static int ma_ease(int v, int dur, bool open)
{
    if (v < 0) v = 0;
    if (v > dur) v = dur;
    int t = (int)(((int64_t)v * 256) / dur);   /* 0..256 */
    if (!open) {
        return (int)(((int64_t)t * t) >> 8);   /* (t/256)^2 * 256 */
    }
    int u = 256 - t;
    return 256 - (int)(((int64_t)u * u) >> 8); /* 256 - (1-t)^2*256 */
}

/* 恢复自动刷新 + 删除顶层图/定时器 + 释放缓冲 */
static void ma_cleanup(void)
{
    if (s_timer) { lv_timer_del(s_timer); s_timer = NULL; }
    if (s_img)   { lv_obj_del(s_img);   s_img   = NULL; }

    lv_timer_t *refr = _lv_disp_get_refr_timer(lv_disp_get_default());
    if (refr) {
        lv_timer_set_period(refr, LV_DISP_DEF_REFR_PERIOD);
        lv_timer_resume(refr);
    }

    lv_obj_clear_flag(lv_scr_act(), LV_OBJ_FLAG_HIDDEN);

    if (s_src)  { heap_caps_free(s_src);  s_src  = NULL; }
    if (s_comp) { heap_caps_free(s_comp); s_comp = NULL; }
    s_phase = MA_PHASE_IDLE;
}

/* 快照新界面到 s_src (临时取消隐藏以获得完整渲染, 快照后恢复隐藏, 不触发刷屏) */
static void ma_snapshot_new(void)
{
    lv_img_dsc_t tmp;   /* 用临时描述符, 避免覆盖 s_img 正在引用的 s_dsc */
    lv_obj_clear_flag(lv_scr_act(), LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(lv_scr_act());
    lv_snapshot_take_to_buf(lv_scr_act(), LV_IMG_CF_TRUE_COLOR, &tmp, s_src, MA_BYTES);
    lv_obj_add_flag(lv_scr_act(), LV_OBJ_FLAG_HIDDEN);
}

static void ma_timer_cb(lv_timer_t *tmr)
{
    (void)tmr;
    bool open = (s_phase == MA_PHASE_OPEN);
    int dur = open ? MA_OPEN_MS : MA_CLOSE_MS;
    int el  = (int)((esp_timer_get_time() - s_t0) / 1000);   /* ms */
    int p   = ma_ease(el, dur, open);                        /* 0..256 */

    int s_q16, radius, alpha;
    if (!open) {
        /* scale 1.0→0.5, radius 0→80, alpha 255→0 */
        s_q16  = MA_SCALE_MAX - (int)(((int64_t)(MA_SCALE_MAX - MA_SCALE_MIN) * p) >> 8);
        radius = (MA_RADIUS_MAX * p) >> 8;
        alpha  = 255 - ((255 * p) >> 8);
    } else {
        /* scale 0.5→1.0, radius 80→0, alpha 0→255 */
        s_q16  = MA_SCALE_MIN + (int)(((int64_t)(MA_SCALE_MAX - MA_SCALE_MIN) * p) >> 8);
        radius = (MA_RADIUS_MAX * (256 - p)) >> 8;
        alpha  = (255 * p) >> 8;
    }

    ma_compose(s_q16, radius, alpha);
    if (s_img) lv_obj_invalidate(s_img);
    lv_refr_now(lv_disp_get_default());

    if (el < dur) return;

    if (!open) {
        /* 全黑: 执行切换工作, 再快照新界面, 转入展开 */
        if (s_work && !s_work_done) {
            s_work_done = true;
            s_work();
        }
        ma_snapshot_new();
        s_phase = MA_PHASE_OPEN;
        s_t0 = esp_timer_get_time();
    } else {
        void (*done)(void) = s_done;
        ma_cleanup();
        if (done) done();
    }
}

bool mode_anim_run(void (*switch_work)(void), void (*done)(void))
{
    if (s_phase != MA_PHASE_IDLE) return false;   /* 不可重入 */

    uint8_t *src  = ma_alloc(MA_BYTES);
    uint8_t *comp = ma_alloc(MA_BYTES);
    if (!src || !comp) goto fail_alloc;

    lv_img_dsc_t tmp;   /* 旧界面快照描述符 (数据在 src) */
    if (lv_snapshot_take_to_buf(lv_scr_act(), LV_IMG_CF_TRUE_COLOR,
                                &tmp, src, MA_BYTES) != LV_RES_OK) {
        goto fail_alloc;
    }

    memcpy(comp, src, MA_BYTES);   /* 首帧即等于当前画面 */

    s_src  = src;
    s_comp = comp;
    memset(&s_dsc, 0, sizeof(s_dsc));
    s_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_dsc.header.w  = MA_W;
    s_dsc.header.h  = MA_H;
    s_dsc.data_size = MA_BYTES;
    s_dsc.data      = s_comp;

    s_img = lv_img_create(lv_layer_top());
    lv_img_set_src(s_img, &s_dsc);
    lv_obj_set_pos(s_img, 0, 0);
    lv_obj_clear_flag(s_img, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_flag(lv_scr_act(), LV_OBJ_FLAG_HIDDEN);

    /* 动画期间暂停自动刷新, 改由定时器手动 lv_refr_now */
    lv_timer_t *refr = _lv_disp_get_refr_timer(lv_disp_get_default());
    if (refr) {
        lv_timer_pause(refr);
        lv_timer_set_period(refr, 1000);
    }

    s_work      = switch_work;
    s_done      = done;
    s_work_done = false;
    s_phase     = MA_PHASE_CLOSE;
    s_t0        = esp_timer_get_time();

    s_timer = lv_timer_create(ma_timer_cb, MA_TICK_MS, NULL);
    if (!s_timer) {
        ma_cleanup();
        return false;
    }
    lv_timer_ready(s_timer);

    ESP_LOGI(TAG, "光圈过渡开始 (close %dms / open %dms)", MA_CLOSE_MS, MA_OPEN_MS);
    return true;

fail_alloc:
    if (src)  heap_caps_free(src);
    if (comp) heap_caps_free(comp);
    ESP_LOGW(TAG, "缓冲分配/快照失败, 降级为淡黑过渡");
    return false;
}
