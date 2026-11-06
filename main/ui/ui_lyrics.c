#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "audio.h"
#include "lyrics.h"
#include "ui_core.h"
#include "ui_lyrics.h"

extern const lv_font_t lv_font_global_16;

/* ── 布局 ── */
#define LYR_AREA_Y      36
#define LYR_AREA_W      172
#define LYR_AREA_H      204
#define LYR_TEXT_X      8
#define LYR_TEXT_W      156
#define LYR_LINE_H      24
#define LYR_GAP         8
#define LYR_WIN         4                    /* 当前条前后各保留 4 条 */
#define LYR_NLABELS     (LYR_WIN * 2 + 1)
#define LYR_MAX_ROWS    10                   /* 单条最多显示 10 行 */
#define LYR_DISP_MAX    65536                /* 折行显示缓冲 (PSRAM) */
#define LYR_TOTAL_MAX   30000                /* lv_coord_t 上限内 */
#define LYR_LATENCY_MS  250                  /* 蓝牙/PCM 缓冲补偿, 实测可调 */
#define LYR_MERGE_MS    50                   /* 时间戳差在此范围内视为同一句的多语言行, 合并显示 */
#define LYR_BLANK_H     12                   /* 空行/占位行占的高度 (半行), 用作段落间距 */
#define LYR_MAX_SEGS    4                    /* 一句最多按文字体系拆成几行 */
#define LYR_MAX_BLOCK_ROWS 12                /* 一个显示块最多几行 (防止极端歌词撑爆) */

/* 透明点击层比歌词区略小, 避免边缘误触/挡住左右侧栏 */
#define LYR_OVL_X       16
#define LYR_OVL_Y       8
#define LYR_OVL_W       140
#define LYR_OVL_H       160

static char *s_raw = NULL;                   /* 原始歌词 UTF-8 (PSRAM) */
static char *s_disp = NULL;                  /* 折行后的显示文本 (PSRAM) */
static size_t s_disp_used = 0;

/* 条目/行高/坐标数组约 3.8KB: 放 PSRAM, 否则内部 DRAM 的 .bss 会链接溢出 */
typedef struct {
    lyric_entry_t entries[LYRICS_MAX_ENTRIES];
    uint16_t      h[LYRICS_MAX_ENTRIES];
    uint16_t      y[LYRICS_MAX_ENTRIES];
    uint32_t      doff[LYRICS_MAX_ENTRIES];
} lyric_store_t;

static lyric_store_t *s_store = NULL;
#define s_entries (s_store->entries)
#define s_h       (s_store->h)
#define s_y       (s_store->y)
#define s_doff    (s_store->doff)

static int      s_count = 0;

static int      s_cur = 0;
static uint32_t s_hash = 0;
static uint32_t s_want_hash = 0;
static uint32_t s_dbg_hash = 0;
static uint8_t  s_retry = 0;
static int64_t  s_missing_since = 0;

static lv_obj_t   *s_area = NULL;
static lv_obj_t   *s_content = NULL;
static lv_obj_t   *s_none = NULL;
static lv_obj_t   *s_overlay = NULL;
static lv_obj_t   *s_labels[LYR_NLABELS];
static lv_timer_t *s_timer = NULL;
static bool        s_visible = false;
static void      (*s_back_cb)(void) = NULL;

/* ── UTF-8 / 语言分类 ── */
static int utf8_next(const char *s, uint32_t *cp)
{
    const unsigned char *p = (const unsigned char *)s;
    if (p[0] < 0x80) { *cp = p[0]; return 1; }
    if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        return 2;
    }
    if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(p[0] & 0x0F) << 12) |
              ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        return 3;
    }
    if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 &&
        (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(p[0] & 0x07) << 18) |
              ((uint32_t)(p[1] & 0x3F) << 12) |
              ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        return 4;
    }
    *cp = p[0];
    return 1;
}

/* ── 文字体系分类 ──
 * 中/日/韩/英/俄 要能分别成行, 所以不能只分 "CJK / 拉丁".
 * 日文是汉字+假名混排, 归为同一段; 中文与韩文/英文/俄文之间会切开. */
enum { SCR_OTHER = 0, SCR_HAN, SCR_KANA, SCR_HANGUL, SCR_LATIN, SCR_CYR, SCR_GREEK };

static int script_of(uint32_t c)
{
    if ((c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) ||
        (c >= 0xF900 && c <= 0xFAFF)) return SCR_HAN;
    if (c >= 0x3040 && c <= 0x30FF) return SCR_KANA;
    if ((c >= 0xAC00 && c <= 0xD7AF) || (c >= 0x1100 && c <= 0x11FF) ||
        (c >= 0x3130 && c <= 0x318F)) return SCR_HANGUL;
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= 0x00C0 && c <= 0x024F) || (c >= 0x1E00 && c <= 0x1EFF)) return SCR_LATIN;
    if (c >= 0x0400 && c <= 0x04FF) return SCR_CYR;
    if (c >= 0x0370 && c <= 0x03FF) return SCR_GREEK;
    return SCR_OTHER;
}

/* 汉字与假名算同一类 (日文), 其余各自一类 */
static int script_class(int s)
{
    return (s == SCR_HAN || s == SCR_KANA) ? 100 : s;
}

typedef struct { int off; int len; } seg_t;

/* 把一条歌词按文字体系切成若干段 (最多 max_segs 段), 返回段数.
 * 数字/标点/空格不参与切换, 自动跟随当前段. */
static int split_scripts(const char *s, int len, seg_t *segs, int max_segs)
{
    int n = 0;
    int i = 0;
    int seg_start = 0;
    int cur = -1;

    while (i < len) {
        uint32_t cp;
        int step = utf8_next(s + i, &cp);
        int scls = script_of(cp);

        if (scls != SCR_OTHER) {          /* 只有语言字符才决定切换 */
            int rc = script_class(scls);
            if (cur < 0) {
                cur = rc;
            } else if (rc != cur && n + 1 < max_segs) {
                segs[n].off = seg_start;
                segs[n].len = i - seg_start;
                n++;
                cur = rc;
                seg_start = i;
            }
        }
        i += step;
    }
    if (n < max_segs) {
        segs[n].off = seg_start;
        segs[n].len = len - seg_start;
        n++;
    }
    return n;
}

/* 把一段 UTF-8 按 156px 折行追加到 s_disp. first=该条是否还没有输出行.
 * 返回行数; 缓冲不足返回 -1. */
static int wrap_segment(const char *seg, int len, int *first)
{
    char tmp[1024];
    if (len > (int)sizeof(tmp) - 1) {
        len = (int)sizeof(tmp) - 1;
        while (len > 0 && ((unsigned char)seg[len] & 0xC0) == 0x80) len--;   /* 不切 UTF-8 中间 */
    }
    if (len <= 0) return 0;
    memcpy(tmp, seg, (size_t)len);
    tmp[len] = '\0';

    int rows = 0;
    int pos = 0;
    while (pos < len && rows < LYR_MAX_ROWS) {
        uint32_t n = _lv_txt_get_next_line(tmp + pos, &lv_font_global_16, 0,
                                           LYR_TEXT_W, NULL, LV_TEXT_FLAG_NONE);
        if (n == 0) break;
        if (pos + (int)n > len) n = (uint32_t)(len - pos);
        if (s_disp_used + n + 2 >= LYR_DISP_MAX) return -1;

        if (!*first) s_disp[s_disp_used++] = '\n';
        memcpy(s_disp + s_disp_used, tmp + pos, n);
        s_disp_used += n;
        *first = 0;
        pos += (int)n;
        rows++;
    }
    return rows;
}

static void update_visibility(void)
{
    if (!s_area) return;
    if (s_count > 0 && s_disp_used > 0) {
        lv_obj_clear_flag(s_content, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_none, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_content, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_none, LV_OBJ_FLAG_HIDDEN);
    }
}

static void lyrics_layout_window(void)
{
    if (!s_area || s_count <= 0) return;

    int first = s_cur - LYR_WIN;
    if (first < 0) first = 0;
    int last = s_cur + LYR_WIN;
    if (last >= s_count) last = s_count - 1;

    for (int slot = 0; slot < LYR_NLABELS; slot++) {
        lv_obj_t *l = s_labels[slot];
        int e = first + slot;
        if (e > last || e >= s_count) {
            lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_label_set_text_static(l, s_disp + s_doff[e]);
        lv_obj_set_pos(l, LYR_TEXT_X, s_y[e]);
        lv_obj_set_size(l, LYR_TEXT_W, s_h[e]);
        lv_obj_set_style_text_color(l, e == s_cur ? COLOR_FG : COLOR_MUTED, 0);
        lv_obj_clear_flag(l, LV_OBJ_FLAG_HIDDEN);
    }
}

static int lyrics_target_y(void)
{
    if (s_count <= 0) return 0;
    return (LYR_AREA_H / 2) - ((int)s_y[s_cur] + (int)s_h[s_cur] / 2);
}

static void set_content_y(void *obj, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)obj, (lv_coord_t)v);
}

static void lyrics_scroll_to(int target, bool animate)
{
    if (!s_content) return;
    lv_anim_del(s_content, set_content_y);
    if (!animate) {
        lv_obj_set_y(s_content, (lv_coord_t)target);
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_content);
    lv_anim_set_exec_cb(&a, set_content_y);
    lv_anim_set_values(&a, lv_obj_get_y(s_content), target);
    lv_anim_set_time(&a, 250);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static int lyrics_index_at(uint32_t ms)
{
    int lo = 0, hi = s_count - 1, ans = 0;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (s_entries[mid].t_ms <= ms) { ans = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    return ans;
}

static void lyrics_apply_time(uint32_t ms, bool force)
{
    if (s_count <= 0) return;
    int idx = lyrics_index_at(ms);
    int old = s_cur;
    s_cur = idx;
    lyrics_layout_window();
    bool animate = s_visible && !force && abs(idx - old) <= 3;
    lyrics_scroll_to(lyrics_target_y(), animate);
}

static void lyrics_load(uint32_t hash)
{
    if (hash != s_want_hash) {   /* 换歌才重置重试 */
        s_want_hash = hash;
        s_retry = 0;
    }
    s_hash = hash;
    s_count = 0;
    s_cur = 0;
    s_disp_used = 0;

    size_t n = 0;
    if (s_raw && s_disp && s_store) {
        n = audio_lyrics_copy(hash, s_raw, LYRICS_BUF_MAX);
        if (!n) n = audio_lyrics_copy(0, s_raw, LYRICS_BUF_MAX);   /* 曲目哈希竞态兜底 */
    }
    if (!n) {
        printf("[歌词] load hash=%08x copy=0 store=%p retry=%u\n",
               (unsigned)hash, (void *)s_store, (unsigned)s_retry);
        /* 刚切歌时音频任务可能还没把歌词放进全局缓冲: 先置 0 让 100ms 定时器
         * 再取几次; 确认没有歌词后再锁定 hash, 避免每 100ms 重复拷贝. */
        if (s_retry < 3 && hash) {
            s_retry++;
            s_hash = 0;
        }
        update_visibility();
        return;
    }

    {
        int bracket_lines = 0;
        for (const char *r = s_raw; *r; ) {
            if (*r == '[') bracket_lines++;
            const char *e = strchr(r, '\n');
            r = e ? (e + 1) : (r + strlen(r));
        }
        const char *q0 = s_raw;
        if ((unsigned char)q0[0] == 0xEF && (unsigned char)q0[1] == 0xBB &&
            (unsigned char)q0[2] == 0xBF) q0 += 3;
        bool ts0 = (q0[0] == '[' && q0[1] >= '0' && q0[1] <= '9');
        printf("[歌词] raw head=%02x %02x %02x %02x len=%u brackets=%d ts0=%d text=%.40s\n",
               (unsigned char)s_raw[0], (unsigned char)s_raw[1],
               (unsigned char)s_raw[2], (unsigned char)s_raw[3],
               (unsigned)strlen(s_raw), bracket_lines, ts0 ? 1 : 0,
               s_raw);
    }

    s_count = lyrics_parse(s_raw, s_entries, LYRICS_MAX_ENTRIES);
    int parsed = s_count;
    printf("[歌词] load hash=%08x copy=%u parse=%d store=%p\n",
           (unsigned)hash, (unsigned)n, s_count, (void *)s_store);
    s_retry = 0;
    if (s_count <= 0) {
        update_visibility();
        return;
    }

    int total = 0;
    int built = 0;
    int i = 0;
    while (i < s_count && built < LYRICS_MAX_ENTRIES) {
        if (s_disp_used + 4 >= LYR_DISP_MAX) break;

        /* 同一时间戳 (差 ≤LYR_MERGE_MS) 的连续条目 = 同一个显示块:
         * LRC 里"原文一行 + 译文一行"用的是同一个时间戳, 必须合并成一句,
         * 否则会显示成两条独立歌词、高亮来回跳. */
        int j = i;
        while (j + 1 < s_count &&
               s_entries[j + 1].t_ms <= s_entries[i].t_ms + LYR_MERGE_MS) {
            j++;
        }

        s_doff[built] = (uint32_t)s_disp_used;
        s_entries[built].t_ms = s_entries[i].t_ms;   /* 压缩后的时间轴 (built <= i, 安全) */

        int first = 1;
        int rows = 0;
        int blank_h = 0;
        int fail = 0;

        for (int k = i; k <= j && !fail; k++) {
            if (s_entries[k].len == 0) {          /* 空行/占位行: 半行间距 */
                blank_h += LYR_BLANK_H;
                continue;
            }
            seg_t segs[LYR_MAX_SEGS];
            int ns = split_scripts(s_entries[k].text, (int)s_entries[k].len,
                                   segs, LYR_MAX_SEGS);
            for (int si = 0; si < ns; si++) {
                int r = wrap_segment(s_entries[k].text + segs[si].off, segs[si].len, &first);
                if (r < 0) { fail = 1; break; }
                rows += r;
                if (rows >= LYR_MAX_BLOCK_ROWS) { rows = LYR_MAX_BLOCK_ROWS; break; }
            }
        }
        if (fail) break;

        int h = rows * LYR_LINE_H + blank_h;
        if (h <= 0) h = LYR_LINE_H;

        s_disp[s_disp_used++] = '\0';
        s_h[built] = (uint16_t)h;
        s_y[built] = (uint16_t)total;
        total += s_h[built] + LYR_GAP;
        built++;
        i = j + 1;
        if (total > LYR_TOTAL_MAX) break;
    }

    s_count = built;
    printf("[歌词] build entries=%d built=%d disp=%u total=%d\n",
           parsed, built, (unsigned)s_disp_used, total);
    if (s_count <= 0) {
        update_visibility();
        return;
    }

    lv_obj_set_size(s_content, LYR_AREA_W, total);
    update_visibility();
    s_cur = 0;
    lyrics_layout_window();
}

static void lyrics_tick(bool force)
{
    song_info_t info;
    int64_t now = esp_timer_get_time();

    if (!song_info_snapshot(&info)) {
        if (s_missing_since == 0) s_missing_since = now;
        if (now - s_missing_since > 500000 && (s_hash || s_count)) {
            s_hash = 0;
            s_count = 0;
            s_disp_used = 0;
            update_visibility();
        }
        return;
    }
    s_missing_since = 0;

    if (info.path_hash != s_dbg_hash) {
        s_dbg_hash = info.path_hash;
        printf("[歌词] tick valid hash=%08x s_hash=%08x count=%d\n",
               (unsigned)info.path_hash, (unsigned)s_hash, s_count);
    }

    if (info.path_hash != s_hash) {
        lyrics_load(info.path_hash);
        force = true;
    }
    if (s_count <= 0) return;

    uint32_t t = info.elapsed_ms > LYR_LATENCY_MS ? info.elapsed_ms - LYR_LATENCY_MS : 0;
    lyrics_apply_time(t, force);
}

static void lyrics_timer_cb(lv_timer_t *tmr)
{
    (void)tmr;
    if (s_visible) lyrics_tick(false);
}

static void overlay_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_back_cb) s_back_cb();
}

void lyrics_view_init(lv_obj_t *parent, void (*back_cb)(void))
{
    s_back_cb = back_cb;

    s_raw = (char *)heap_caps_malloc(LYRICS_BUF_MAX, MALLOC_CAP_SPIRAM);
    s_disp = (char *)heap_caps_malloc(LYR_DISP_MAX, MALLOC_CAP_SPIRAM);
    s_store = (lyric_store_t *)heap_caps_malloc(sizeof(lyric_store_t), MALLOC_CAP_SPIRAM);
    printf("[歌词] init raw=%p disp=%p store=%p psram_free=%u\n",
           (void *)s_raw, (void *)s_disp, (void *)s_store,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    if (!s_raw || !s_disp || !s_store) {
        printf("[歌词] PSRAM 分配失败, 歌词功能不可用\n");
    }

    s_area = lv_obj_create(parent);
    lv_obj_set_pos(s_area, 0, LYR_AREA_Y);
    lv_obj_set_size(s_area, LYR_AREA_W, LYR_AREA_H);
    lv_obj_set_style_bg_opa(s_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_area, 0, 0);
    lv_obj_set_style_pad_all(s_area, 0, 0);
    lv_obj_set_style_radius(s_area, 0, 0);
    lv_obj_clear_flag(s_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_area, LV_SCROLLBAR_MODE_OFF);
    /* 容器本身不接收点击: 普通 lv_obj 默认带 CLICKABLE, 歌词区空白处会静默
     * 吞掉触摸(没挂事件, 点了没反应), 压住下方进度条的判定带. 只保留 s_overlay. */
    lv_obj_clear_flag(s_area, LV_OBJ_FLAG_CLICKABLE);

    s_content = lv_obj_create(s_area);
    lv_obj_set_pos(s_content, 0, 0);
    lv_obj_set_size(s_content, LYR_AREA_W, 1);
    lv_obj_set_style_bg_opa(s_content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_content, 0, 0);
    lv_obj_set_style_pad_all(s_content, 0, 0);
    lv_obj_set_style_radius(s_content, 0, 0);
    lv_obj_clear_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_content, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(s_content, LV_OBJ_FLAG_CLICKABLE);   /* 同上: 内容层也不吃点击 */
    lv_obj_add_flag(s_content, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < LYR_NLABELS; i++) {
        lv_obj_t *l = lv_label_create(s_content);
        lv_obj_set_pos(l, LYR_TEXT_X, 0);
        lv_obj_set_size(l, LYR_TEXT_W, LYR_LINE_H);
        lv_obj_set_style_text_font(l, &lv_font_global_16, 0);
        lv_obj_set_style_text_color(l, COLOR_MUTED, 0);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_all(l, 0, 0);
        lv_obj_set_style_bg_opa(l, LV_OPA_TRANSP, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_clear_flag(l, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
        s_labels[i] = l;
    }

    s_none = lv_label_create(s_area);
    lv_label_set_text(s_none, "无歌词");
    lv_obj_set_style_text_font(s_none, &lv_font_global_16, 0);
    lv_obj_set_style_text_color(s_none, COLOR_MUTED, 0);
    lv_obj_align(s_none, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_none, LV_OBJ_FLAG_HIDDEN);

    /* 透明点击层: 盖住歌词区中心, 点击返回封面; 不盖底部进度条/左右侧栏 */
    s_overlay = lv_btn_create(s_area);
    lv_obj_set_pos(s_overlay, LYR_OVL_X, LYR_OVL_Y);
    lv_obj_set_size(s_overlay, LYR_OVL_W, LYR_OVL_H);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_shadow_width(s_overlay, 0, 0);
    lv_obj_set_style_radius(s_overlay, 0, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_overlay, overlay_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_add_flag(s_area, LV_OBJ_FLAG_HIDDEN);

    s_timer = lv_timer_create(lyrics_timer_cb, 100, NULL);
    if (s_timer) lv_timer_pause(s_timer);
}

void lyrics_view_destroy(void)
{
    if (s_timer) { lv_timer_del(s_timer); s_timer = NULL; }
    if (s_content) lv_anim_del(s_content, set_content_y);
    if (s_area) { lv_obj_del(s_area); s_area = NULL; }

    s_content = NULL;
    s_none = NULL;
    s_overlay = NULL;
    for (int i = 0; i < LYR_NLABELS; i++) s_labels[i] = NULL;

    if (s_raw) { heap_caps_free(s_raw); s_raw = NULL; }
    if (s_disp) { heap_caps_free(s_disp); s_disp = NULL; }
    if (s_store) { heap_caps_free(s_store); s_store = NULL; }

    s_count = 0;
    s_cur = 0;
    s_disp_used = 0;
    s_hash = 0;
    s_want_hash = 0;
    s_dbg_hash = 0;
    s_retry = 0;
    s_missing_since = 0;
    s_visible = false;
    s_back_cb = NULL;
}

void lyrics_view_show(void)
{
    if (!s_area) return;
    s_visible = true;
    lv_obj_clear_flag(s_area, LV_OBJ_FLAG_HIDDEN);
    update_visibility();
    if (s_timer) {
        lv_timer_resume(s_timer);
        lv_timer_ready(s_timer);
    }
    lyrics_tick(true);
}

void lyrics_view_hide(void)
{
    s_visible = false;
    if (s_area) lv_obj_add_flag(s_area, LV_OBJ_FLAG_HIDDEN);
    if (s_timer) lv_timer_pause(s_timer);
}

void lyrics_view_refresh_now(void)
{
    lyrics_tick(true);
}

bool lyrics_view_has_lyrics(void)
{
    return s_count > 0;
}
