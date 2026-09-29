#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"
#include "lvgl.h"
#include "settings.h"
#include "sys_monitor.h"
#include "atomic_utils.h"
#include "text_encoding.h"
#include "ui_core.h"
#include "ui_novel.h"
#include "ui_player.h"
#include "menu.h"

extern const lv_font_t lv_font_global_16;
extern const lv_font_t lv_font_montserrat_14;
extern const lv_font_t lv_font_montserrat_12;

/* ── 布局 ── */
#define NOVEL_READ_X     2
#define NOVEL_READ_Y     40
#define NOVEL_READ_W     168
#define NOVEL_READ_H     238
#define NOVEL_PAGE_Y     282
#define NOVEL_BTN_W      78
#define NOVEL_BTN_H      32
#define NOVEL_TEXT_W     (NOVEL_READ_W - 10)   /* 文本显示宽度 (与 label 一致, 折行用) */
#define PAGE_BYTES       1024   /* 每页读取字节数 */
#define PAGE_STACK_MAX   256    /* 页首偏移栈容量 (RAM 内, 不落盘) */
/* 原始字节转换 UTF-8 后的缓冲: GBK 双字节→UTF-8 三字节, 最坏 1.5x, 取 2x 富余 */
#define U8_MAX           (PAGE_BYTES * 2 + 8)
#define DISP_MAX         (U8_MAX + 256)        /* 预折行显示缓冲 (每行最多 +1 个 '\n') */
#define NOVEL_PAGE_OFFSET 0

/* 方案B: 把预折行后的文本按 NOVEL_BLOCK_LINES 行一组切成多个小 label.
 * 每个控件固定宽高、WRAP、且清除 SCROLLABLE (见下方方案说明注释). */
#define NOVEL_BLOCK_LINES 4      /* 每个文本控件承载的行数 (3~4, 折中值) */
#define NOVEL_MAX_BLOCKS  40     /* 文本控件池上限 (异常文件保护) */
#define NOVEL_LINE_SPACE  2      /* 行间距, 与旧 label 保持一致 */

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
#define VBAT_PCT_MAX     4.15f

/* ── 静态状态 ── */
static lv_obj_t   *s_root       = NULL;  /* 全屏根容器 */
static lv_obj_t   *s_pct_lbl    = NULL;  /* 顶部中间: 阅读百分比 */
static lv_obj_t   *s_bat_fill   = NULL;  /* 顶部右侧: 电池填充条 */
static lv_obj_t   *s_blocks[NOVEL_MAX_BLOCKS];  /* 阅读文本控件池 (方案B: 每 3~4 行一个控件) */
static int         s_block_pool = 0;     /* 池中已创建的控件数 */
static int         s_block_used = 0;     /* 当前页实际使用的控件数 */
static lv_obj_t   *s_read_cont  = NULL;  /* 阅读滚动容器 */
static lv_timer_t *s_bat_timer  = NULL;
static lv_obj_t   *s_dialog     = NULL;  /* "文件不存在/需重新扫描" 弹窗 */

static FILE       *s_book       = NULL;  /* 当前小说文件 */
static char        s_book_path[512] = {0};  /* 当前小说真实路径 (进度存 NVS / 去重) */
static uint32_t    s_book_size  = 0;     /* 文件总字节数 */
static uint32_t    s_page_start = 0;     /* 当前页起始偏移 */
static uint32_t    s_page_next  = 0;     /* 下一页起始偏移 */
static uint32_t    s_page_stack[PAGE_STACK_MAX];  /* 页首偏移历史栈 */
static int         s_page_sp    = 0;     /* 栈顶指针 */

static char        s_buf[PAGE_BYTES + 8];  /* 原始页数据缓冲 (含结尾 '\0') */
static char        s_u8[U8_MAX];           /* 识别+转换后的 UTF-8 文本 */
static char        s_disp[DISP_MAX];       /* 预折行后的显示串 (插入 '\n') */

static void novel_open_path(const char *real_path, bool interactive);   /* 按真实路径打开 (interactive=失败时弹框) */
static void novel_restore_last(void);                 /* 恢复上次打开的小说 */

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

/* ============================================================================
 * 小说文本渲染性能方案 (历经实测得出, 请勿随意改回"单个大 label")
 * ----------------------------------------------------------------------------
 * 背景: 一页约 45 行(约 1000 字), 渲染区 160x218 仅能显示约 9~11 行, 需要滚动.
 *
 * [未优化] 单个 LV_LABEL_LONG_WRAP 大 label:
 *   - 每帧滚动都要按宽度重新换行(reflow), 实测约 300~500ms/s, 为最初最大瓶颈.
 *
 * [优化1] 预折行(按宽度插 '\n', novel_wrap_text) + LV_LABEL_LONG_CLIP:
 *   - reflow 从约 300~500ms/s 降到约 14ms/s. 但暴露出新的最大瓶颈:
 *   - CLIP 使 label->expand=1, lv_draw_label_impl() 走 EXPAND 分支, 每次绘制都对
 *     【整段文本】调 lv_txt_get_size() 求最宽行 (src/draw/lv_draw_label.c:134-144),
 *     进而逐行 lv_txt_get_width -> 逐字字形(gdsc)查询.
 *   - 实测 tgsize 约 520ms/s(126 次/s), tgwidth 约 549ms/s(5670 行/s = 45 行 x 126);
 *     gdsc/gid 约 97% 都源自这里.
 *
 * [优化2] 显式定高(novel_freeze_height) -> 无效:
 *   - 该测量发生在"绘制"阶段, 与 label 高度无关, 定高消不掉它.
 *   - 另有约 84 次/s 的 tgsize 来自 label 的布局/自身尺寸查询
 *     (STYLE_CHANGED/SIZE_CHANGED -> lv_label_refr_text, 或 GET_SELF_SIZE), 同样与高度无关.
 *
 * [结论] 只要"一个比可视区长的 label 被反复绘制/滚动", LVGL 每次都会量整段文本.
 *   改回 WRAP 只是把测量从 lv_txt_get_size 换成 _lv_txt_get_next_line, 依旧逐字.
 *
 * [最终方案 B] 将预折行后的文本按 NOVEL_BLOCK_LINES(3~4) 行切成多个小 label 放入滚动容器:
 *   - 每个 label 固定宽(=NOVEL_TEXT_W)固定高(=行数 x (行高+行距)), long_mode=WRAP,
 *     并 clear LV_OBJ_FLAG_SCROLLABLE; 绝不使用 SIZE_CONTENT.
 *   - 离屏控件被 lv_obj_redraw() 裁剪(core/lv_refr.c:140-143), 不绘制不测量;
 *     绘制循环也在超出 clip 下沿时立即返回(src/draw/lv_draw_label.c:383).
 *   - 于是每次只测量可见的约 9~11 行, 成本降约一个数量级.
 *
 * [为何是 3~4 行/控件, 而非每行一个, 也非回收池]:
 *   - 绘制成本与"每行一个"几乎相同(离屏都被裁剪), 但控件数由约 45 降到约 12~15,
 *     RAM 更小(约 3KB vs 约 10KB), 翻页时创建更快.
 *   - 回收池(可见+2 个)内存恒定, 但需自写 LV_EVENT_SCROLL 回收逻辑, 复杂度最高.
 *   - K 取 3~4 是折中: K 太大会重新引入"整控件测量", K 太小则控件数过多.
 * ==========================================================================*/

/* 预折行: 把 src[0..valid) (已转好的 UTF-8) 按 label 宽度折成多行, 行尾插入 '\n' 存入 s_disp.
 * 本函数每页只执行一次; 返回值供 novel_display_blocks 分组. */

static int novel_wrap_text(const char *src, size_t valid)
{
    const lv_font_t *font = &lv_font_global_16;
    size_t pos = 0, dp = 0;
    int lines = 0;

    while (pos < valid && dp + 1 < DISP_MAX) {
        uint32_t line = _lv_txt_get_next_line(&src[pos], font, 0, NOVEL_TEXT_W,
                                              NULL, LV_TEXT_FLAG_NONE);
        if (line == 0) break;
        if (pos + line > valid) line = (uint32_t)(valid - pos);   /* 不越过有效数据 */

        for (uint32_t i = 0; i < line && dp + 1 < DISP_MAX; i++) {
            s_disp[dp++] = src[pos + i];
        }
        pos += line;
        lines++;

        /* 本行若非以换行结尾 (按宽度折的), 补 '\n' 固定该行, 避免 CLIP 下被再拼接 */
        if (dp > 0) {
            char last = s_disp[dp - 1];
            if (last != '\n' && last != '\r' && pos < valid && dp + 1 < DISP_MAX) {
                s_disp[dp++] = '\n';
            }
        }
    }
    s_disp[dp] = '\0';
    return lines;
}

/* 取第 i 个文本控件, 池不足则按需创建 (方案B).
 * 固定宽高、WRAP、清除 SCROLLABLE, 避免滚动时被当作滚动候选而触发 GET_SELF_SIZE 整段测宽. */
static lv_obj_t *novel_block_get(int i)
{
    if (i < 0 || i >= NOVEL_MAX_BLOCKS) return NULL;
    if (i >= s_block_pool) {
        lv_obj_t *lbl = lv_label_create(s_read_cont);
        lv_obj_set_width(lbl, NOVEL_TEXT_W);
        lv_obj_set_style_text_font(lbl, &lv_font_global_16, 0);
        lv_obj_set_style_text_color(lbl, COLOR_FG, 0);
        lv_obj_set_style_text_line_space(lbl, NOVEL_LINE_SPACE, 0);
        /* WRAP(expand=0): 绘制时用对象宽度, 不做整段测宽 (行已由 novel_wrap_text 折好) */
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(lbl, LV_SCROLLBAR_MODE_OFF);
        s_blocks[i] = lbl;
        s_block_pool = i + 1;
    }
    return s_blocks[i];
}

/* 隐藏 [from, pool) 区间内多余控件 */
static void novel_block_hide_from(int from)
{
    for (int i = from; i < s_block_pool; i++) {
        if (s_blocks[i]) lv_obj_add_flag(s_blocks[i], LV_OBJ_FLAG_HIDDEN);
    }
}

/* 方案B: 把预折行后的 s_disp 按 K 行一组切成多个小 label 并纵向排布.
 * 就地修改 s_disp: 在控件边界把结尾 '\n' 换成 '\0' 作为字符串结束 (lv_label_set_text 会复制内容). */
static void novel_display_blocks(int total)
{
    if (!s_read_cont) return;
    if (total <= 0) { novel_block_hide_from(0); s_block_used = 0; return; }

    /* 每控件行数: 默认 NOVEL_BLOCK_LINES; 行数异常多时放大, 以限制控件总数 */
    int K = NOVEL_BLOCK_LINES;
    if (total > NOVEL_MAX_BLOCKS * K)
        K = (total + NOVEL_MAX_BLOCKS - 1) / NOVEL_MAX_BLOCKS;
    if (K < 1) K = 1;

    lv_coord_t step = lv_font_get_line_height(&lv_font_global_16) + NOVEL_LINE_SPACE;
    char *p = s_disp;
    lv_coord_t y = 0;
    int b = 0;

    while (*p && b < NOVEL_MAX_BLOCKS) {
        int cnt = K;
        if (b * K + cnt > total) cnt = total - b * K;

        char *block_start = p;
        for (int j = 0; j < cnt; j++) {
            char *nl = strchr(p, '\n');
            if (!nl) { p += strlen(p); break; }
            p = nl + 1;
        }
        if (*p && p > block_start) p[-1] = '\0';   /* 切断本组结尾的 '\n' */

        lv_obj_t *lbl = novel_block_get(b);
        if (!lbl) break;
        lv_label_set_text(lbl, block_start);
        lv_obj_set_height(lbl, (lv_coord_t)cnt * step);
        lv_obj_set_pos(lbl, 0, y);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_HIDDEN);
        y += (lv_coord_t)cnt * step;
        b++;
    }

    novel_block_hide_from(b);
    s_block_used = b;
}

/* 显示单条提示信息 (错误/占位/读完), 复用第 0 个控件 */
static void novel_show_message(const char *msg)
{
    if (!s_read_cont) return;
    lv_obj_t *lbl = novel_block_get(0);
    if (!lbl) return;

    lv_label_set_text(lbl, msg);
    lv_point_t sz;
    lv_txt_get_size(&sz, msg, &lv_font_global_16, 0, NOVEL_LINE_SPACE, NOVEL_TEXT_W, LV_TEXT_FLAG_NONE);
    lv_obj_set_height(lbl, sz.y > 0 ? sz.y : lv_font_get_line_height(&lv_font_global_16));
    lv_obj_set_pos(lbl, 0, 0);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_HIDDEN);

    novel_block_hide_from(1);
    s_block_used = 1;
    lv_obj_scroll_to_y(s_read_cont, 0, LV_ANIM_OFF);
}

/* ── 文件不存在提示弹窗 (小说专属, 与音乐 show_file_not_found 对应) ── */
static void novel_dialog_delete(void)
{
    if (s_dialog) { lv_obj_del(s_dialog); s_dialog = NULL; }
}

/* 确认: 关弹窗 + 请求重新扫描 (复用 sys_monitor 手动重扫流程) */
static void novel_rescan_cb(lv_event_t *e)
{
    (void)e;
    novel_dialog_delete();
    g_sd_manual_rescan = SD_RESCAN_NOVEL;
    printf("[NOVEL] 请求重新扫描\n");
}

/* 弹出"文件不存在"对话框 (含重新扫描确认按钮) */
static void novel_show_file_not_found(void)
{
    novel_dialog_delete();

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
    lv_obj_add_event_cb(btn, novel_rescan_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, "确认");
    lv_obj_set_style_text_font(btn_lbl, &lv_font_global_16, 0);
    lv_obj_set_style_text_color(btn_lbl, lv_color_black(), 0);
    lv_obj_center(btn_lbl);
}


/* 读取并显示 offset 起始的一页 */
static void novel_read_page(uint32_t offset)
{
    if (!s_book || !s_read_cont) return;
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
        novel_show_message("已读完所有内容");
        s_page_start = s_book_size;
        s_page_next  = s_book_size;
        novel_update_percent();
        return;
    }

    /* 自动识别编码并转 UTF-8: 去 BOM / UTF-8 原样 / GBK 转码;
     * consumed = 实际消耗的原始字节 (已对齐完整字符边界, 含剥离的 BOM) */
    size_t consumed = n;
    int u8len = text_to_utf8((const uint8_t *)s_buf, n, s_u8, sizeof(s_u8), &consumed);
    if (u8len < 0) u8len = 0;

    /* 控制字符替换为空格 (换行/制表保留); '\r' 也替换, 避免 CRLF 被当成两条换行 */
    for (size_t i = 0; i < (size_t)u8len; i++) {
        unsigned char c = (unsigned char)s_u8[i];
        if (c == '\r') {
            s_u8[i] = ' ';
        } else if (c < 0x20 && c != '\n' && c != '\t') {
            s_u8[i] = ' ';
        }
    }

    int lines = novel_wrap_text(s_u8, (size_t)u8len);   /* 预折行 (每页一次) */
    novel_display_blocks(lines);          /* 方案B: 按 3~4 行拆成多个小 label */
    lv_obj_scroll_to_y(s_read_cont, 0, LV_ANIM_OFF);

    s_page_start = offset;
    s_page_next  = offset + (uint32_t)consumed;

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
    novel_progress_save(s_book_path, s_page_start);   /* 翻页后写 NVS */
}

static void novel_prev_page(void)
{
    if (!s_book) return;
    uint32_t start;
    if (s_page_sp > 0) {
        start = s_page_stack[--s_page_sp];
    } else {
        start = 0;   /* 无历史: 回到开头 */
    }
    novel_read_page(start);
    novel_progress_save(s_book_path, s_page_start);   /* 翻页后写 NVS */
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

    /* 左上: 文件浏览器按钮 (透明, 放大点击区, 后面会置顶覆盖阅读框, 便于点按) */
    lv_obj_t *btn_menu = make_icon_btn(s_root, 0, 0, 56, 56, LV_SYMBOL_LIST);
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

    /* 菜单按钮置顶: 覆盖在阅读框之上 (按钮透明, 不影响观感, 只扩大点击区) */
    lv_obj_move_foreground(btn_menu);

    /* 文本控件按需创建 (方案B, 见文件上方方案说明); 先显示占位提示 */
    novel_show_message("请点击左上角选择小说文件");

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
    s_book_path[0] = '\0';
}

void ui_novel_show(void)
{
    novel_build();
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_root);
    if (s_bat_timer) lv_timer_resume(s_bat_timer);
    if (s_pct_lbl && !s_book) lv_label_set_text(s_pct_lbl, "0.0%");
    novel_restore_last();   /* 开机/切换进小说模式: 自动恢复上次打开的小说 */
}

void ui_novel_destroy(void)
{
    novel_dialog_delete();
    novel_close_file();
    if (s_bat_timer) { lv_timer_del(s_bat_timer); s_bat_timer = NULL; }
    if (s_root) { lv_obj_del(s_root); s_root = NULL; }
    s_pct_lbl = NULL;
    s_bat_fill = NULL;
    for (int i = 0; i < s_block_pool; i++) s_blocks[i] = NULL;
    s_block_pool = 0;
    s_block_used = 0;
    s_read_cont = NULL;
}

/* 按真实路径打开小说文件, 并恢复该文件的阅读进度;
 * interactive=true (用户手动点击) 时打开失败弹"重新扫描"框, 否则只显示行内提示 */
static void novel_open_path(const char *real_path, bool interactive)
{
    if (!real_path || !real_path[0]) return;
    if (!atomic_load_bool(&g_sd_ready)) return;

    if (!s_root) novel_build();

    novel_close_file();

    sd_fs_lock();
    s_book = fopen(real_path, "rb");
    if (s_book) {
        if (fseek(s_book, 0, SEEK_END) == 0) {
            long sz = ftell(s_book);
            s_book_size = (sz > 0) ? (uint32_t)sz : 0;
        }
    }
    sd_fs_unlock();

    if (!s_book) {
        if (interactive) {
            novel_show_file_not_found();   /* 手动点击失败: 弹框询问是否重扫 */
        } else {
            novel_show_message("无法打开小说文件");   /* 自动恢复失败: 静默行内提示 */
        }
        if (s_pct_lbl) lv_label_set_text(s_pct_lbl, "0.0%");
        printf("[NOVEL] open failed: %s\n", real_path);
        return;
    }

    novel_dialog_delete();   /* 成功打开: 清掉可能残留的失败弹框 */

    strncpy(s_book_path, real_path, sizeof(s_book_path) - 1);
    s_book_path[sizeof(s_book_path) - 1] = '\0';
    last_novel_save(s_book_path);   /* 记住本次打开, 供下次自动恢复 */

    printf("[NOVEL] open: %s (%lu bytes)\n", s_book_path, (unsigned long)s_book_size);

    /* 恢复该文件上次的阅读进度; 无记录/越界则从头开始 */
    uint32_t start = 0;
    if (novel_progress_load(s_book_path, &start) && start <= s_book_size) {
        printf("[NOVEL] 恢复进度: %lu\n", (unsigned long)start);
    } else {
        start = 0;
    }
    novel_read_page(start);
}

/* 恢复上次打开的小说 (仅当尚未打开文件且 SD 就绪) */
static void novel_restore_last(void)
{
    if (s_book) return;                               /* 已有打开的文件 */
    if (!atomic_load_bool(&g_sd_ready)) return;       /* SD 未就绪, 待插入后再恢复 */

    char saved[512];
    if (!last_novel_load(saved, sizeof(saved))) return;

    novel_open_path(saved, false);   /* 自动恢复: 失败不弹框 */
}

void ui_novel_open(const char *group, const char *name)
{
    if (!group || !name) return;

    char path[512];
    fs_build_real_path(group, name, path, sizeof(path));
    novel_open_path(path, true);   /* 用户手动点击: 失败弹框 */
}

/* 当前打开小说的 group/name: 由真实路径反推 (去掉开头 '/' → "sdcard/..." 后按最后一个 '/' 切分) */
bool ui_novel_current(char *group, size_t group_size, char *name, size_t name_size)
{
    if (!s_book_path[0]) return false;

    const char *p = s_book_path;
    if (*p == '/') p++;                 /* /sdcard/小说/foo/a.txt → sdcard/小说/foo/a.txt */

    const char *slash = strrchr(p, '/');
    if (!slash || slash == p) return false;

    if (group && group_size > 0) {
        size_t gl = (size_t)(slash - p);
        if (gl >= group_size) gl = group_size - 1;
        memcpy(group, p, gl);
        group[gl] = '\0';
    }
    if (name && name_size > 0) {
        const char *bn = slash + 1;
        size_t nl = strlen(bn);
        if (nl >= name_size) nl = name_size - 1;
        memcpy(name, bn, nl);
        name[nl] = '\0';
    }
    return true;
}

/* SD 插入: 小说模式下恢复上次打开的小说 (由 ui_shell 的 SD 监视调用) */
void ui_novel_on_sd_ready(void)
{
    if (!s_root) return;   /* 非小说模式 (UI 未构建): 不恢复 */
    novel_restore_last();
}

/* SD 拔出: 关闭当前打开的小说文件 (不销毁 UI) */
void ui_novel_on_sd_remove(void)
{
    novel_dialog_delete();
    novel_close_file();
    novel_show_message("SD 卡已拔出");
    if (s_pct_lbl) lv_label_set_text(s_pct_lbl, "0.0%");
}

bool ui_novel_is_active(void)
{
    return s_root != NULL;
}
