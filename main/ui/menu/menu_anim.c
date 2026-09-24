#include <string.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "menu.h"

/* ────────────────────────────────────────────
 * 通用面板 打开/关闭 展开动画 (截图到 PSRAM + 顶部图片层)
 * 文件浏览器与蓝牙列表共用
 *
 * 原理: 先把主界面和面板各截图到 PSRAM, 隐藏真实控件,
 * 在 LVGL 顶层放两张图片 (背景 + 面板), 用定时器按"裁切窗口"
 * 逐帧合成动画图, 手动刷新显示; 动画结束恢复真实控件。
 * ──────────────────────────────────────────── */
#define ANIM_FS_START    30   /* 动画起始尺寸 (最小可见尺寸) */
#define ANIM_FS_MS_OPEN  150   /* 打开动画: 先快后慢 (ease-out) */
#define ANIM_FS_MS_CLOSE 100   /* 关闭动画: 线性 */
#define ANIM_PANEL_RADIUS 5    /* 面板圆角半径 (两容器统一写死) */

static const panel_anim_cfg_t *s_anim_cfg    = NULL;   /* 当前动画配置 */
static lv_obj_t      *s_anim_bg_img  = NULL;   /* 顶部背景图 (主界面截图) */
static lv_obj_t      *s_anim_fs_img  = NULL;   /* 面板展开图 */
static lv_img_dsc_t   s_anim_dsc_main;         /* 主界面全屏截图描述符 */
static lv_img_dsc_t   s_anim_dsc_fs;           /* 面板截图描述符 */
static lv_img_dsc_t   s_anim_dsc_buf;          /* 合成缓冲 (背景+面板裁切) 描述符 */
static uint8_t       *s_anim_buf_main = NULL;  /* 主界面截图 (PSRAM) */
static uint8_t       *s_anim_buf_fs   = NULL;  /* 面板截图 (PSRAM) */
static uint8_t       *s_anim_buf_anim = NULL;  /* PSRAM 合成缓冲 */
static int32_t        s_anim_open     = 1;     /* 1=打开动画 0=关闭动画 */
static int64_t        s_anim_t0       = 0;     /* 动画起始 esp_timer us */
static lv_timer_t    *s_anim_timer    = NULL;  /* 动画驱动定时器 */

static volatile int64_t s_anim_compose_us = 0; /* 最近一帧 compose 耗时 */
static volatile int64_t s_anim_refr_us    = 0; /* 最近一帧 LVGL 刷屏耗时 */
static int64_t  s_prev_frame_us = 0;           /* 上一帧时间戳 (瞬时 fps) */
static bool     s_prev_frame_ok = false;       /* 首帧无前一帧, fps 打印 0 */
static int s_anim_prev_w = 0, s_anim_prev_h = 0; /* 上一帧显示区域尺寸 (增量复用) */
static bool s_anim_first_frame = true;          /* 首帧需全量填充 (buf 未初始化) */
/* 本次 compose 的变化区域 (buf 坐标, 分两块 A=垂直列条 B=水平行条, 不合并避免推方块) */
static lv_area_t s_anim_chg_a;
static lv_area_t s_anim_chg_b;
static bool      s_anim_chg_valid = false;

/* 分配动画缓冲: 优先 PSRAM, 失败回退内部 RAM */
static void *fs_anim_alloc(uint32_t bytes)
{
    void *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!p) p = heap_caps_malloc(bytes, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    return p;
}

/* 单次合成缓冲:
 * 动画面板区域 = 显示区域 (w,h)。y>=h 的行整行取背景;
 * y<h 的行内, 显示区域圆角内取容器像素, 圆角外取背景。
 * 每个目标像素只写一次 (原为两次复制: 整块背景 + 容器覆盖)。
 * anchor_right 时容器贴右 (蓝牙从右上展开), 否则贴左 (文件浏览器)。 */
static void fs_anim_compose(int w, int h)
{
    int ptw = s_anim_cfg->w, pth = s_anim_cfg->h;
    int r = ANIM_PANEL_RADIUS;
    int ox = s_anim_cfg->anchor_right ? (ptw - w) : 0;  /* 容器区在目标缓冲的 x 偏移 */

    for (int y = 0; y < pth; y++) {
        uint8_t *dst = s_anim_buf_anim + y * ptw * 2;
        uint8_t *bg  = s_anim_buf_main + ((s_anim_cfg->y + y) * 172 + s_anim_cfg->x) * 2;

        /* y >= h: 容器未覆盖, 整行背景 */
        if (y >= h) {
            memcpy(dst, bg, ptw * 2);
            continue;
        }

        /* 该行显示区域圆角内 x 区间 (基于当前显示区域宽 w, 圆心在四角 (r,r)) */
        int rx0 = 0, rx1 = w - 1;
        if (y < r) {
            /* 顶角行: 圆心 y=r, 该行距圆心 dy = r-y */
            int dy = r - y;
            if (dy > 0) {
                lv_sqrt_res_t q;
                lv_sqrt(r * r - dy * dy, &q, 0x8000);
                int s = q.i;
                rx0 = r - s; rx1 = w - 1 - (r - s);
            }
        } else if (y >= h - r) {
            /* 底角行: 圆心 y = h-1-r, 该行距圆心 dy = y-(h-1-r) */
            int dy = y - (h - 1 - r);
            if (dy > 0) {
                lv_sqrt_res_t q;
                lv_sqrt(r * r - dy * dy, &q, 0x8000);
                int s = q.i;
                rx0 = r - s; rx1 = w - 1 - (r - s);
            }
        }
        if (rx0 < 0) rx0 = 0;
        if (rx1 > w - 1) rx1 = w - 1;
        if (rx0 > rx1) rx0 = rx1;

        /* 容器区圆角内区间在目标缓冲的 x 范围 (目标 ox 与源偏移一致, 直接对齐) */
        int cx0 = ox + rx0, cx1 = ox + rx1;

        /* 复制: 圆角内取容器, 圆角外取背景, 逐段 memcpy */
        int pos = 0;
        while (pos < ptw) {
            if (pos < cx0) {
                memcpy(dst + pos * 2, bg + pos * 2, (cx0 - pos) * 2);
                pos = cx0;
            } else if (pos <= cx1) {
                memcpy(dst + pos * 2, s_anim_buf_fs + y * ptw * 2 + pos * 2,
                       (cx1 - pos + 1) * 2);
                pos = cx1 + 1;
            } else {
                memcpy(dst + pos * 2, bg + pos * 2, (ptw - pos) * 2);
                pos = ptw;
            }
        }
    }
}

/* 单行/单区段复制: 把目标缓冲 [tx0,tx1) 区间按当前帧 (w,h) 的圆角规则填好。
 * 打开动画时复用旧帧已填充区域, 只重写新增的右侧条/底部条 (背景不用重新复制);
 * 关闭动画时把回收条改回背景。 */
static void fs_anim_fill_seg(int y, int tx0, int tx1, int w, int h)
{
    if (tx0 >= tx1) return;
    int ptw = s_anim_cfg->w;
    int r = ANIM_PANEL_RADIUS;
    int ox = s_anim_cfg->anchor_right ? (ptw - w) : 0;  /* 容器区在目标缓冲的 x 偏移 */

    uint8_t *dst = s_anim_buf_anim + y * ptw * 2;
    uint8_t *bg  = s_anim_buf_main + ((s_anim_cfg->y + y) * 172 + s_anim_cfg->x) * 2;
    uint8_t *fs  = s_anim_buf_fs + y * ptw * 2;

    /* y >= h: 容器未覆盖, 整段背景 */
    if (y >= h) {
        memcpy(dst + tx0 * 2, bg + tx0 * 2, (tx1 - tx0) * 2);
        return;
    }

    /* 该行显示区域圆角内 x 区间 (窗口坐标 [0,w)) */
    int rx0 = 0, rx1 = w - 1;
    if (y < r) {
        int dy = r - y;
        if (dy > 0) {
            lv_sqrt_res_t q;
            lv_sqrt(r * r - dy * dy, &q, 0x8000);
            int s = q.i;
            rx0 = r - s; rx1 = w - 1 - (r - s);
        }
    } else if (y >= h - r) {
        int dy = y - (h - 1 - r);
        if (dy > 0) {
            lv_sqrt_res_t q;
            lv_sqrt(r * r - dy * dy, &q, 0x8000);
            int s = q.i;
            rx0 = r - s; rx1 = w - 1 - (r - s);
        }
    }
    if (rx0 < 0) rx0 = 0;
    if (rx1 > w - 1) rx1 = w - 1;
    if (rx0 > rx1) rx0 = rx1;

    /* 圆角区间 -> 目标缓冲 x 范围 */
    int cx0 = ox + rx0, cx1 = ox + rx1;

    /* 复制 [tx0,tx1): 圆角内取容器, 圆角外取背景 */
    int pos = tx0;
    while (pos < tx1) {
        if (pos < cx0) {
            int e = (tx1 < cx0) ? tx1 : cx0;
            memcpy(dst + pos * 2, bg + pos * 2, (e - pos) * 2);
            pos = e;
        } else if (pos <= cx1) {
            int e = (tx1 < cx1 + 1) ? tx1 : cx1 + 1;
            memcpy(dst + pos * 2, fs + pos * 2, (e - pos) * 2);
            pos = e;
        } else {
            memcpy(dst + pos * 2, bg + pos * 2, (tx1 - pos) * 2);
            pos = tx1;
        }
    }
}

/* 增量合成: 相比上一帧 (s_anim_prev_w,h), 只重写变化区域, 复用旧帧像素。
 * 原理: 展开/收缩是"裁切窗口"而非整图缩放, 只有新增条/回收条变化。
 * - 中间行 (无圆角): 只写新增的列条, 其余像素旧帧已正确。
 * - 顶/底圆角行 (各 r 行): 圆角位置随 w/h 移动, 整行重写最省心。
 * anchor_right 时窗口坐标与目标缓冲 x 有偏移, 由 fs_anim_fill_seg 处理。 */

/* 把 buf 坐标变化区域映射到屏幕坐标并 invalidate (不推全屏, 只推变化矩形) */
static void fs_anim_inv_area(const lv_area_t *buf_area)
{
    lv_area_t scr;
    lv_area_set(&scr,
                s_anim_cfg->x + buf_area->x1,
                s_anim_cfg->y + buf_area->y1,
                s_anim_cfg->x + buf_area->x2,
                s_anim_cfg->y + buf_area->y2);
    lv_obj_invalidate_area(s_anim_fs_img, &scr);
}

static void fs_anim_compose_incr(int w, int h)
{
    int ptw = s_anim_cfg->w;
    int r = ANIM_PANEL_RADIUS;
    int w_prev = s_anim_prev_w, h_prev = s_anim_prev_h;
    int ox = s_anim_cfg->anchor_right ? (ptw - w) : 0;
    bool ar = s_anim_cfg->anchor_right;

    /* 计算变化区域 (buf 坐标, 分两块矩形 A=列条 B=水平条, 不合并避免 L 型变方块) */
    if (s_anim_open) {
        /* ── 打开: 新增列条 + 新增底部条 ── */
        /* 块 A (列条): anchor_left 左扩 r 覆盖顶部右上角圆角移动, anchor_right 右缘固定 */
        if (ar) {
            lv_area_set(&s_anim_chg_a, ptw - w, 0, ptw - w_prev - 1, h - 1);
        } else {
            int x0 = w_prev - r; if (x0 < 0) x0 = 0;
            lv_area_set(&s_anim_chg_a, x0, 0, w - 1, h - 1);
        }
        /* 块 B (底部条): 旧底角行变中间行 + 新增底部, 起点 h_prev-r 覆盖角落 */
        int by = (h_prev - r > 0) ? (h_prev - r) : 0;
        if (ar) {
            lv_area_set(&s_anim_chg_b, ptw - w_prev, by, ptw - 1, h - 1);
        } else {
            lv_area_set(&s_anim_chg_b, 0, by, w_prev - 1, h - 1);
        }
    } else {
        /* ── 关闭: 回收列条 + 回收底部条 ── */
        if (ar) {
            lv_area_set(&s_anim_chg_a, ptw - w_prev, 0, ptw - w - 1, h - 1);
        } else {
            int x0 = w - r; if (x0 < 0) x0 = 0;
            lv_area_set(&s_anim_chg_a, x0, 0, w_prev - 1, h - 1);
        }
        int by = (h - r > 0) ? (h - r) : 0;
        if (ar) {
            lv_area_set(&s_anim_chg_b, ptw - w_prev, by, ptw - 1, h_prev - 1);
        } else {
            lv_area_set(&s_anim_chg_b, 0, by, w_prev - 1, h_prev - 1);
        }
    }
    s_anim_chg_valid = true;

    if (s_anim_open) {
        /* ── 打开: 区域扩张 ── */
        int new_w = w - w_prev;

        /* 1. 新增列条 (仅中间行): anchor_left 在 [w_prev,w), anchor_right 在 [ptw-w,ptw-w_prev) */
        if (new_w > 0) {
            int sx0 = s_anim_cfg->anchor_right ? (ptw - w) : w_prev;
            int sx1 = sx0 + new_w;
            int y_mid_lo = r;
            int y_mid_hi = (h_prev - r > 0) ? (h_prev - r) : 0;   /* 两帧都是中间行 */
            for (int y = y_mid_lo; y < y_mid_hi; y++) {
                fs_anim_fill_seg(y, sx0, sx1, w, h);
            }
        }
        /* 2. 顶部圆角行整行重写 */
        for (int y = 0; y < r && y < h; y++) {
            fs_anim_fill_seg(y, ox, ox + w, w, h);
        }
        /* 3. 旧底角行 + 新增底部条: [h_prev-r, h) 整行重写 */
        int b0 = (h_prev - r > 0) ? (h_prev - r) : 0;
        for (int y = b0; y < h; y++) {
            fs_anim_fill_seg(y, ox, ox + w, w, h);
        }
    } else {
        /* ── 关闭: 区域收缩 ── */
        int shrink_w = w_prev - w;
        int shrink_h = h_prev - h;

        /* 1. 回收右/左列条 (仅中间行) 改回背景 */
        if (shrink_w > 0) {
            int rx0 = s_anim_cfg->anchor_right ? (ptw - w_prev) : w;
            int rx1 = rx0 + shrink_w;
            int y_mid_lo = r;
            int y_mid_hi = (h - r > 0) ? (h - r) : 0;   /* 两帧都是中间行 */
            for (int y = y_mid_lo; y < y_mid_hi; y++) {
                fs_anim_fill_seg(y, rx0, rx1, w, h);
            }
        }
        /* 2. 回收底部条: [h, h_prev) 整行背景 */
        if (shrink_h > 0) {
            int wrow = s_anim_cfg->anchor_right ? ptw : w_prev;
            for (int y = h; y < h_prev; y++) {
                fs_anim_fill_seg(y, s_anim_cfg->anchor_right ? (ptw - w_prev) : 0,
                                 wrow, w, h);
            }
        }
        /* 3. 顶部圆角行整行重写 (覆盖到旧容器区右缘, 否则 [w,w_prev) 遗留旧像素) */
        {
            int x0 = s_anim_cfg->anchor_right ? (ptw - w_prev) : 0;
            int x1 = x0 + w_prev;
            for (int y = 0; y < r && y < h; y++) {
                fs_anim_fill_seg(y, x0, x1, w, h);
            }
        }
        /* 4. 底角行整行重写 (圆角随 h 上移, 同样覆盖到旧容器区右缘) */
        {
            int b0 = (h - r > 0) ? (h - r) : 0;
            int x0 = s_anim_cfg->anchor_right ? (ptw - w_prev) : 0;
            int x1 = x0 + w_prev;
            for (int y = b0; y < h; y++) {
                fs_anim_fill_seg(y, x0, x1, w, h);
            }
        }
    }

    s_anim_prev_w = w;
    s_anim_prev_h = h;
}

static void fs_anim_timer_cb(lv_timer_t *tmr)
{
    int64_t dur_ms = s_anim_open ? ANIM_FS_MS_OPEN : ANIM_FS_MS_CLOSE;

    int64_t elaps = esp_timer_get_time() - s_anim_t0;  /* us */
    int64_t v = elaps / 1000;                           /* ms 原始进度 */
    if (v > dur_ms) v = dur_ms;
    if (v < 0) v = 0;

    /* 进度曲线: 打开=先快后慢 (ease-out 三次方), 关闭=线性 */
    int64_t p;
    if (s_anim_open) {
        /* p = 1 - (1 - v/dur)^3 */
        double t = (double)v / (double)dur_ms;
        p = (int64_t)((1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t)) * dur_ms);
    } else {
        p = v;
    }

    int w, h;
    int ptw = s_anim_cfg->w, pth = s_anim_cfg->h;
    if (s_anim_open) {
        w = ANIM_FS_START + (int)((int64_t)(ptw - ANIM_FS_START) * p / dur_ms);
        h = ANIM_FS_START + (int)((int64_t)(pth - ANIM_FS_START) * p / dur_ms);
    } else {
        w = ptw - (int)((int64_t)(ptw - ANIM_FS_START) * p / dur_ms);
        h = pth - (int)((int64_t)(pth - ANIM_FS_START) * p / dur_ms);
    }
    if (w < ANIM_FS_START) w = ANIM_FS_START;
    if (h < ANIM_FS_START) h = ANIM_FS_START;
    if (w > ptw) w = ptw;
    if (h > pth) h = pth;

    int64_t c0 = esp_timer_get_time();
    bool first = s_anim_first_frame;
    if (s_anim_first_frame) {
        fs_anim_compose(w, h);   /* 首帧: 全量填充 (buf 未初始化) */
        s_anim_first_frame = false;
        lv_obj_invalidate(s_anim_fs_img);
    } else {
        fs_anim_compose_incr(w, h);
        /* 分块 invalidate: 只推变化区域 (两块矩形, 避免 L 型变方块推全屏) */
        if (s_anim_chg_valid) {
            fs_anim_inv_area(&s_anim_chg_a);
            fs_anim_inv_area(&s_anim_chg_b);
        }
    }
    s_anim_prev_w = w;
    s_anim_prev_h = h;
    s_anim_compose_us = esp_timer_get_time() - c0;

    /* 手动触发同步刷新, 并测量刷屏耗时 (LVGL 内部绘制 + flush) */
    int64_t r0 = esp_timer_get_time();
    lv_refr_now(lv_disp_get_default());
    s_anim_refr_us = esp_timer_get_time() - r0;

    if (first) {
        printf("[ANIM] first frame at %.2fms compose=%.2fms refr=%.2fms\n",
               elaps / 1000.0f, s_anim_compose_us / 1000.0f,
               s_anim_refr_us / 1000.0f);
    }

    if (v >= dur_ms) {
        lv_timer_del(tmr);
        s_anim_timer = NULL;
        if (s_anim_open) {
            /* 打开完成: 真实面板已就绪, 删除顶部图片层并恢复显示 */
            lv_obj_del(s_anim_bg_img);
            lv_obj_del(s_anim_fs_img);
            s_anim_bg_img = NULL;
            s_anim_fs_img = NULL;
            lv_obj_clear_flag(lv_scr_act(), LV_OBJ_FLAG_HIDDEN);
        } else {
            /* 关闭完成: 删除顶部图片层 + 真实面板, 恢复主界面 */
            lv_obj_del(s_anim_bg_img);
            lv_obj_del(s_anim_fs_img);
            s_anim_bg_img = NULL;
            s_anim_fs_img = NULL;
            if (s_anim_cfg->close_real) s_anim_cfg->close_real();
            lv_obj_clear_flag(lv_scr_act(), LV_OBJ_FLAG_HIDDEN);
        }
        if (s_anim_buf_main) { heap_caps_free(s_anim_buf_main); s_anim_buf_main = NULL; }
        if (s_anim_buf_fs)   { heap_caps_free(s_anim_buf_fs);   s_anim_buf_fs   = NULL; }
        if (s_anim_buf_anim) { heap_caps_free(s_anim_buf_anim); s_anim_buf_anim = NULL; }

        /* 动画结束: 恢复 LVGL 自动刷新 */
        lv_timer_t *refr_t = _lv_disp_get_refr_timer(lv_disp_get_default());
        if (refr_t) {
            lv_timer_set_period(refr_t, LV_DISP_DEF_REFR_PERIOD);
            lv_timer_resume(refr_t);
        }
    }
}

static void fs_anim_start(int open)
{
    if (s_anim_timer) {
        lv_timer_del(s_anim_timer);
        s_anim_timer = NULL;
        /* 清理上一个未完成动画的资源 */
        if (s_anim_bg_img) { lv_obj_del(s_anim_bg_img); s_anim_bg_img = NULL; }
        if (s_anim_fs_img) { lv_obj_del(s_anim_fs_img); s_anim_fs_img = NULL; }
        lv_obj_clear_flag(lv_scr_act(), LV_OBJ_FLAG_HIDDEN);
        if (s_anim_buf_main) { heap_caps_free(s_anim_buf_main); s_anim_buf_main = NULL; }
        if (s_anim_buf_fs)   { heap_caps_free(s_anim_buf_fs);   s_anim_buf_fs   = NULL; }
        if (s_anim_buf_anim) { heap_caps_free(s_anim_buf_anim); s_anim_buf_anim = NULL; }
        /* 上一个动画被中断, 恢复自动刷新 */
        lv_timer_t *old_refr = _lv_disp_get_refr_timer(lv_disp_get_default());
        if (old_refr) {
            lv_timer_set_period(old_refr, LV_DISP_DEF_REFR_PERIOD);
            lv_timer_resume(old_refr);
        }
    }
    s_anim_open = open;
    s_anim_t0 = esp_timer_get_time();
    /* 每次动画开始: 瞬时 fps 状态复位 (首帧打印 0) */
    s_prev_frame_ok = false;
    s_prev_frame_us = 0;
    /* 打开: 首帧需全量填充 buf (全新分配)。关闭: buf 已在 panel_anim_close 全量填好,
     * prev 初始化为全尺寸, 首帧(全展开)走增量且无变化。 */
    s_anim_first_frame = (open != 0);
    s_anim_prev_w = open ? 0 : s_anim_cfg->w;
    s_anim_prev_h = open ? 0 : s_anim_cfg->h;

    /* 动画期间: 暂停 LVGL 自动刷新, 改由动画定时器手动触发 (测刷屏耗时) */
    lv_timer_t *refr_t = _lv_disp_get_refr_timer(lv_disp_get_default());
    if (refr_t) {
        lv_timer_pause(refr_t);
        lv_timer_set_period(refr_t, 1000);   /* 兜底 1s 1 帧 */
    }

    s_anim_timer = lv_timer_create(fs_anim_timer_cb, 20, NULL);
    lv_timer_ready(s_anim_timer);
}

/* 打开动画通用入口: 在菜单点击回调调用 (此时真实面板尚未创建) */
void panel_anim_open(const panel_anim_cfg_t *cfg)
{
    s_anim_cfg = cfg;
    int pw = cfg->w, ph = cfg->h;
    int64_t t0 = esp_timer_get_time();

    /* 1. 主界面截图 (面板未创建, 画面干净) */
    s_anim_buf_main = fs_anim_alloc(172 * 320 * 2);
    if (!s_anim_buf_main) { cfg->open_real(); return; }
    if (lv_snapshot_take_to_buf(lv_scr_act(), LV_IMG_CF_TRUE_COLOR,
                                &s_anim_dsc_main, s_anim_buf_main,
                                172 * 320 * 2) != LV_RES_OK) {
        heap_caps_free(s_anim_buf_main); s_anim_buf_main = NULL;
        cfg->open_real();
        return;
    }
    int64_t t1 = esp_timer_get_time();

    /* 2. 创建真实面板 */
    cfg->open_real();
    int64_t t2 = esp_timer_get_time();

    /* 3. 面板截图 */
    s_anim_buf_fs = fs_anim_alloc(pw * ph * 2);
    s_anim_buf_anim = fs_anim_alloc(pw * ph * 2);
    if (!s_anim_buf_fs || !s_anim_buf_anim) {
        heap_caps_free(s_anim_buf_main); s_anim_buf_main = NULL;
        heap_caps_free(s_anim_buf_fs);   s_anim_buf_fs   = NULL;
        heap_caps_free(s_anim_buf_anim); s_anim_buf_anim = NULL;
        lv_obj_clear_flag(lv_scr_act(), LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (lv_snapshot_take_to_buf(*cfg->cont_ref, LV_IMG_CF_TRUE_COLOR,
                                &s_anim_dsc_fs, s_anim_buf_fs,
                                pw * ph * 2) != LV_RES_OK) {
        heap_caps_free(s_anim_buf_main); s_anim_buf_main = NULL;
        heap_caps_free(s_anim_buf_fs);   s_anim_buf_fs   = NULL;
        heap_caps_free(s_anim_buf_anim); s_anim_buf_anim = NULL;
        lv_obj_clear_flag(lv_scr_act(), LV_OBJ_FLAG_HIDDEN);
        return;
    }
    int64_t t3 = esp_timer_get_time();
    printf("[ANIM] open main_snap=%.2fms open_real=%.2fms panel_snap=%.2fms total=%.2fms\n",
           (t1 - t0) / 1000.0f, (t2 - t1) / 1000.0f, (t3 - t2) / 1000.0f,
           (t3 - t0) / 1000.0f);

    /* 4. 隐藏主界面 + 面板 (一个 flag, 隐藏即不绘制子树) */
    lv_obj_add_flag(lv_scr_act(), LV_OBJ_FLAG_HIDDEN);

    /* 5. 顶部层: 背景主界面图 */
    s_anim_bg_img = lv_img_create(lv_layer_top());
    lv_img_set_src(s_anim_bg_img, &s_anim_dsc_main);
    lv_obj_set_pos(s_anim_bg_img, 0, 0);

    /* 6. 顶部层: 面板展开图 (位于面板坐标) */
    memset(&s_anim_dsc_buf, 0, sizeof(s_anim_dsc_buf));
    s_anim_dsc_buf.header.w = pw;
    s_anim_dsc_buf.header.h = ph;
    s_anim_dsc_buf.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_anim_dsc_buf.data_size = pw * ph * 2;
    s_anim_dsc_buf.data = s_anim_buf_anim;
    s_anim_fs_img = lv_img_create(lv_layer_top());
    lv_img_set_src(s_anim_fs_img, &s_anim_dsc_buf);
    lv_obj_set_pos(s_anim_fs_img, cfg->x, cfg->y);

    /* 7. 启动展开动画 */
    fs_anim_start(1);
}

/* 关闭动画通用入口: 在 overlay 点击回调调用 (真实面板可见) */
void panel_anim_close(const panel_anim_cfg_t *cfg)
{
    if (!*cfg->overlay_ref) return;
    s_anim_cfg = cfg;
    int pw = cfg->w, ph = cfg->h;

    /* 1. 当前面板截图 (可能已翻页/进目录) */
    s_anim_buf_fs = fs_anim_alloc(pw * ph * 2);
    s_anim_buf_anim = fs_anim_alloc(pw * ph * 2);
    s_anim_buf_main = fs_anim_alloc(172 * 320 * 2);
    if (!s_anim_buf_fs || !s_anim_buf_anim || !s_anim_buf_main) {
        if (s_anim_buf_fs)   { heap_caps_free(s_anim_buf_fs);   s_anim_buf_fs = NULL; }
        if (s_anim_buf_anim) { heap_caps_free(s_anim_buf_anim); s_anim_buf_anim = NULL; }
        if (s_anim_buf_main) { heap_caps_free(s_anim_buf_main); s_anim_buf_main = NULL; }
        cfg->close_real();
        return;
    }
    if (lv_snapshot_take_to_buf(*cfg->cont_ref, LV_IMG_CF_TRUE_COLOR,
                                &s_anim_dsc_fs, s_anim_buf_fs,
                                pw * ph * 2) != LV_RES_OK) {
        cfg->close_real();
        heap_caps_free(s_anim_buf_fs);   s_anim_buf_fs   = NULL;
        heap_caps_free(s_anim_buf_anim); s_anim_buf_anim = NULL;
        heap_caps_free(s_anim_buf_main); s_anim_buf_main = NULL;
        return;
    }

    /* 2. 临时隐藏面板 overlay 截图干净主界面, 再恢复 (同帧无闪烁) */
    lv_obj_add_flag(*cfg->overlay_ref, LV_OBJ_FLAG_HIDDEN);
    if (lv_snapshot_take_to_buf(lv_scr_act(), LV_IMG_CF_TRUE_COLOR,
                                &s_anim_dsc_main, s_anim_buf_main,
                                172 * 320 * 2) != LV_RES_OK) {
        lv_obj_clear_flag(*cfg->overlay_ref, LV_OBJ_FLAG_HIDDEN);
        cfg->close_real();
        heap_caps_free(s_anim_buf_fs);   s_anim_buf_fs   = NULL;
        heap_caps_free(s_anim_buf_anim); s_anim_buf_anim = NULL;
        heap_caps_free(s_anim_buf_main); s_anim_buf_main = NULL;
        return;
    }
    lv_obj_clear_flag(*cfg->overlay_ref, LV_OBJ_FLAG_HIDDEN);

    /* 3. 隐藏主界面 + 面板 */
    lv_obj_add_flag(lv_scr_act(), LV_OBJ_FLAG_HIDDEN);

    /* 4. 顶部层图片: 背景 + 初始全展开面板 */
    s_anim_bg_img = lv_img_create(lv_layer_top());
    lv_img_set_src(s_anim_bg_img, &s_anim_dsc_main);
    lv_obj_set_pos(s_anim_bg_img, 0, 0);

    memset(&s_anim_dsc_buf, 0, sizeof(s_anim_dsc_buf));
    s_anim_dsc_buf.header.w = pw;
    s_anim_dsc_buf.header.h = ph;
    s_anim_dsc_buf.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_anim_dsc_buf.data_size = pw * ph * 2;
    s_anim_dsc_buf.data = s_anim_buf_anim;
    s_anim_fs_img = lv_img_create(lv_layer_top());
    lv_img_set_src(s_anim_fs_img, &s_anim_dsc_buf);
    lv_obj_set_pos(s_anim_fs_img, cfg->x, cfg->y);

    fs_anim_compose(pw, ph);

    /* 5. 启动收拢动画 */
    fs_anim_start(0);
}
