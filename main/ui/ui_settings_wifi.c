/* 设置页的「连接 WiFi / 检查更新」子流程
 *
 *   连接 WiFi (竖屏) → 密码 (横屏, 顺时针 90°) → 正在连接 (竖屏) → 正在检查更新 (竖屏)
 *                                              ↘ 连接失败 (竖屏, 2 秒) → 退出流程
 *   正在检查更新 → 版本页 (当前/最新 + 「更新」) → 下载/写入进度页 → 重启进新固件
 *                ↘ 已是最新 / 检查失败 / 更新失败 (竖屏提示页, 到点自动退出)
 *
 * 所有提示页 (连接失败/检查失败/已是最新/更新失败/更新完成) 都是同一个
 * notice_build(), 到点和按电源键都是同一个 flow_exit() —— 加一种结果不需要
 * 再写一套"删页面 + 停定时器 + 收尾"的代码.
 *
 * 为什么连接失败要退出整个流程, 而不是退回列表
 * ----------------------------------------------
 * 横屏页要一块 2KB 的"内部 DMA 中转缓冲"(见 ui_core.c 的 s_rot_stage), 这块缓冲只在
 * WIFI_EV_READY 那一刻好拿: 此时 esp_wifi_init 刚结束、列表页还没建, DMA 池有 ~12KB.
 * 等列表页建完就只剩 ~900B (实测持有时 943B/最大连续块 304B), 中途再也抢不到 2KB.
 * 而"连接失败 -> 回列表 -> 重扫 -> 再进密码页"这条路径恰恰要求中途重新申请,
 * 于是必然花屏 (Failed to allocate priv TX buffer).
 * 现在改成失败即退出: 每次重试都是一次全新的流程入口, 也就回到了那个 12KB 的时刻.
 *
 * 统一出口
 * --------
 * 电源键 / 左上角返回 / 失败页 2 秒超时, 三个入口全部只调 ui_wifi_back() 或 flow_exit(),
 * 最终都走同一个 flow_exit() -> wifi_mgr_release() -> WIFI_EV_RELEASED ->
 * ui_wifi_destroy(). 任何新增的退出路径都不许自己拼收尾代码, 否则迟早漏掉一处.
 *
 * 记住密码
 * --------
 * 连成功一次就把 "SSID + 密码" 写进 NVS (见 app/wifi_cred.c). 下次在列表里选中同一台
 * 点「连接」, 直接用记住的密码连, 密码页整个跳过 —— 连带着那块 2KB 横屏缓冲都不用申请.
 * 只有失败原因是"密码错" (IDF 的 4WAY_HANDSHAKE_TIMEOUT) 才删掉这条记录;
 * 找不到 AP / 超时都保留, 那不是密码的问题. 删掉之后下次进流程会重新弹密码页.
 *
 * 为什么密码页要横屏
 * ------------------
 * 键盘 10 列, 竖屏 172 宽每个键只有 15px, 按不准; 转 90° 后 320 宽, 每个键约 28px.
 * 用的是 LVGL 的软件旋转 (lv_disp_set_rotation): 面板分辨率不变, 只是把每块脏区在
 * 推送前转 90° —— 触点坐标 LVGL 会一起转 (lv_indev.c:347), 触摸驱动不用改.
 * 注意 ui_core.c 的 flush 在旋转态必须等 DMA 排空 (旋转会复用/原地改写同一块缓冲),
 * 否则横屏页会串行错位.
 *
 * 触摸死区
 * --------
 * 面板物理上下各有一条读不到的窄边 (ui_core.c 把原始 y 的 5..310 拉伸到 0..319),
 * 转 90° 之后这两条边落在横屏的左右两侧. 所以横屏页四边各留 8px, 键盘最外侧的键
 * 不会压在任何一条可能读不到的边上.
 *
 * 不许中途退出
 * ------------
 * 扫描/连接/释放期间 s_busy=true: 电源键与左上角返回全部无反应 (另外还置了
 * power_mgr_set_ui_inhibit, 连无操作熄屏一起挡住), 没有弹窗, 就是按了没反应.
 * "连接失败"提示页是唯一 s_busy=false 的过渡页: 那 2 秒里按电源键可以立刻走.
 * WiFi 的清理只在流程真正结束时做一次, 且由 wifi_mgr 的任务跑完手上的操作后执行. */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "lvgl.h"

#include "power_mgr.h"
#include "ui_core.h"
#include "ui_settings_wifi.h"
#include "update.h"
#include "wifi_cred.h"
#include "wifi_mgr.h"

static const char *TAG = "SET_WIFI";

/* 配色 (与 ui_settings.c 的白底一套) */
#define W_BG      lv_color_hex(0xFFFFFF)
#define W_TEXT    lv_color_hex(0x111111)
#define W_MUTED   lv_color_hex(0x888888)
#define W_LINE    lv_color_hex(0xE0E0E0)
#define W_BTN_BG  lv_color_hex(0xF5F5F5)
#define W_BTN_BR  lv_color_hex(0xD0D0D0)
#define W_PRESSED lv_color_hex(0xEFEFEF)
#define W_SEL     lv_color_hex(0xEDEDED)
#define W_DOT     lv_color_hex(0xDDDDDD)

#define W_TITLE_H  48
#define W_ROW_H    38
#define W_LIST_H   196      /* 列表可视高度 (5 行多一点, 其余滚动) */
#define W_BTN_Y    272

/* 横屏 (顺时针 90°) 的逻辑尺寸: 面板 172x320 转过来就是 320x172 */
#define LAN_W 320
#define LAN_H 172

extern const lv_font_t lv_font_global_16;
extern const lv_font_t lv_font_montserrat_14;
extern const lv_font_t lv_font_montserrat_18;

typedef enum {
    WP_NONE = 0,
    WP_BOOT,        /* 正在初始化 WiFi (极简占位页) */
    WP_LIST,        /* 连接 WiFi: SSID 列表 + 扫描/连接 */
    WP_PASS,        /* 密码 (横屏) */
    WP_CONNECTING,  /* 正在连接 */
    WP_CHECK,       /* 正在查版本 (GET releases) */
    WP_VER,         /* 当前版本 / 最新版本 + 「更新」按钮 */
    WP_PROG,        /* 下载 + 写入 (进度条, 期间不许退出) */
    WP_NOTICE,      /* 结果提示 (失败原因/已是最新/更新完成), 到点自动退出 */
} wpage_t;

/* 提示页停留时长 */
#define NOTICE_HOLD_MS     2000   /* 连接失败 / 已是最新 */
#define NOTICE_HOLD_ERR    3000   /* 带原因的失败 */
#define NOTICE_HOLD_DONE   1500   /* 更新完成, 即将重启 */

/* WiFi 断开原因里的"密码错": IDF 的 WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT.
 * 只有这个原因才删掉 NVS 里记的密码 —— 热点没开/信号差/超时都不是密码的问题,
 * 删了等于白记一次, 下次还得重输. */
#define REASON_WRONG_PASS  15

static wpage_t     s_page;
static bool        s_busy;          /* 有 WiFi 操作在跑: 一切退出入口无效 */
static bool        s_session;       /* 本次进入流程后有一次 WiFi 会话没结束 */
static bool        s_leaving;       /* 已经决定退出, flow_exit 只走一次 */
static bool        s_pass_from_nvs; /* 本次连接用的是 NVS 里记住的密码 (没走密码页) */
static bool        s_cred_dropped;  /* 记忆的密码被判无效并删除 (失败页要说明) */
static lv_timer_t *s_timer;         /* 事件轮询 (100ms) + 小动画 */
static void      (*s_end_cb)(void);

static char s_sel[33];              /* 选中的 SSID */
static int  s_sel_idx = -1;
static char s_pass[65];

/* 初始化 WiFi 时的极简占位页.
 * 为什么要它: esp_wifi_init 需要 ~28KB 的 "DMA 可用内部堆", 而完整的「连接 WiFi」
 * 列表页 (顶栏/返回键/标题/行容器/提示/状态行/两个按钮) 要吃掉将近 10KB ——
 * 先建页再 init 会直接把 WiFi 掐死在 "Init static rx buffer" 那一步. 所以这里
 * 只放一个容器 + 一行字 (~1KB), 等 WIFI_EV_READY 再销毁它、去建真正的列表页. */
static lv_obj_t *s_boot_pg;
static lv_obj_t *s_boot_lbl;

/* 连接 WiFi 页 */
static lv_obj_t *s_list_pg;
static lv_obj_t *s_rows;            /* 可滚动的行容器 */
static lv_obj_t *s_hint;            /* 列表为空/扫描中的提示 (与行容器同级, 不参与滚动) */
static lv_obj_t *s_status;          /* 状态行 */
static lv_obj_t *s_back_btn;
static lv_obj_t *s_btn_scan;
static lv_obj_t *s_btn_conn;

/* 行对象缓存 (选中态只改两行配色, 不重建列表) */
static lv_obj_t *s_row[WIFI_MGR_MAX_APS];
static lv_obj_t *s_row_chk[WIFI_MGR_MAX_APS];
static int       s_row_n;

/* 密码页 */
static lv_obj_t *s_pass_pg;
static lv_obj_t *s_ta;
static lv_obj_t *s_kb;

/* 正在连接 / 检查更新 / 版本页 / 进度页 / 结果提示 */
static lv_obj_t   *s_conn_pg;
static lv_obj_t   *s_dots;          /* 「正在…」后面的点点 (连接页和检查页共用) */
static lv_obj_t   *s_check_pg;
static lv_obj_t   *s_ver_pg;
static lv_obj_t   *s_ver_cur;
static lv_obj_t   *s_ver_new;
static lv_obj_t   *s_prog_pg;
static lv_obj_t   *s_prog_title;
static lv_obj_t   *s_prog_pct;
static lv_obj_t   *s_prog_sub;
static lv_obj_t   *s_notice_pg;
static lv_timer_t *s_notice_timer;  /* 提示页的一次性定时器 */
static bool        s_restart_after_exit;  /* 升级成功: 释放完 WiFi 直接重启 */
static int         s_tick;
static int         s_prog_tick;

/* 前向声明 (页面之间互相跳转) */
static void list_build(void);
static void pass_build(void);
static void pass_delete(void);
static void conn_page_delete(void);
static void prog_delete(void);
static void ver_delete(void);
static void notice_delete(void);
static void notice_build(const char *title, const char *l1, const char *l2,
                         const char *l3, int hold_ms);
static void start_connect(void);
static void flow_exit(const char *why);
static void notice_exit_cb(lv_timer_t *t);

/* ── 小工具 ── */

/* 整屏白底容器 (尺寸取当前逻辑分辨率: 竖屏 172x320, 横屏 320x172) */
static lv_obj_t *page_create(void)
{
    lv_obj_t *o = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, lv_disp_get_hor_res(NULL), lv_disp_get_ver_res(NULL));
    lv_obj_set_pos(o, 0, 0);
    lv_obj_set_style_bg_color(o, W_BG, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *mk_label(lv_obj_t *parent, const char *txt, lv_color_t color,
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

static lv_obj_t *mk_btn(lv_obj_t *parent, const char *txt, lv_coord_t x, lv_coord_t y,
                        lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_set_style_bg_color(b, W_BTN_BG, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, W_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(b, lv_color_hex(0xEDEDED), LV_STATE_DISABLED);
    lv_obj_set_style_border_color(b, W_BTN_BR, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);

    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &lv_font_global_16, 0);
    lv_obj_set_style_text_color(l, W_TEXT, 0);
    lv_obj_center(l);
    return b;
}

/* 1px 分隔线 */
static void mk_line(lv_obj_t *parent, lv_coord_t y, lv_coord_t w)
{
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_remove_style_all(line);
    lv_obj_set_pos(line, 0, y);
    lv_obj_set_size(line, w, 1);
    lv_obj_set_style_bg_color(line, W_LINE, 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
}

/* ── 状态 / 忙碌 ── */

static void set_status(const char *txt)
{
    if (s_status) lv_label_set_text(s_status, txt);
}

/* busy: 禁止退出 + 禁止无操作熄屏 + 三个按钮置灰 */
static void set_busy(bool busy)
{
    if (s_busy == busy) return;
    s_busy = busy;
    power_mgr_set_ui_inhibit(busy);

    lv_obj_t *btns[3] = { s_back_btn, s_btn_scan, s_btn_conn };
    for (int i = 0; i < 3; i++) {
        if (!btns[i]) continue;
        if (busy) lv_obj_add_state(btns[i], LV_STATE_DISABLED);
        else      lv_obj_clear_state(btns[i], LV_STATE_DISABLED);
    }
    ESP_LOGI(TAG, "WiFi 操作%s", busy ? "开始 (禁止退出)" : "结束");
}

/* ── 列表行 ── */

static void rows_clear(void)
{
    for (int i = 0; i < WIFI_MGR_MAX_APS; i++) {
        s_row[i] = NULL;
        s_row_chk[i] = NULL;
    }
    s_row_n = 0;
    if (s_rows) lv_obj_clean(s_rows);
}

/* 空列表 / 扫描中的提示 */
static void list_hint(const char *txt)
{
    if (!s_hint) return;
    lv_label_set_text(s_hint, txt);
    lv_obj_clear_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
}

static void row_style(int i, bool sel)
{
    if (i < 0 || i >= s_row_n || !s_row[i]) return;
    lv_obj_set_style_bg_color(s_row[i], sel ? W_SEL : W_BG, 0);
    if (s_row_chk[i]) {
        lv_obj_set_style_text_color(s_row_chk[i], sel ? COLOR_ACCENT : W_DOT, 0);
    }
}

static void row_cb(lv_event_t *e)
{
    if (s_busy) return;
    lv_obj_t *row = lv_event_get_target(e);
    int i = (int)(intptr_t)lv_obj_get_user_data(row) - 1;
    if (i < 0 || i >= s_row_n) return;

    if (s_sel_idx >= 0 && s_sel_idx != i) row_style(s_sel_idx, false);
    s_sel_idx = i;
    row_style(i, true);
    snprintf(s_sel, sizeof s_sel, "%s", wifi_mgr_ap_ssid(i));
    power_mgr_notify_activity();
    ESP_LOGI(TAG, "选中网络: \"%s\"%s", s_sel, wifi_mgr_ap_open(i) ? " (开放)" : "");
}

/* 按 wifi_mgr 的扫描结果重建列表 */
static void list_fill(void)
{
    rows_clear();
    int n = wifi_mgr_ap_count();
    if (n > WIFI_MGR_MAX_APS) n = WIFI_MGR_MAX_APS;

    if (n == 0) {
        list_hint("没有搜到网络\n按「扫描」重新搜索");
        return;
    }
    if (s_hint) lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < n; i++) {
        lv_obj_t *row = lv_btn_create(s_rows);
        lv_obj_set_size(row, TFT_HOR_RES, W_ROW_H);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_bg_color(row, W_BG, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, W_PRESSED, LV_STATE_PRESSED);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(row, W_LINE, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_user_data(row, (void *)(intptr_t)(i + 1));
        lv_obj_add_event_cb(row, row_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *l = lv_label_create(row);
        lv_label_set_text(l, wifi_mgr_ap_ssid(i));
        lv_obj_set_style_text_font(l, &lv_font_global_16, 0);
        lv_obj_set_style_text_color(l, W_TEXT, 0);
        lv_obj_set_pos(l, 10, 9);
        lv_obj_set_width(l, 108);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);

        lv_obj_t *c = lv_label_create(row);
        lv_label_set_text(c, LV_SYMBOL_OK);
        lv_obj_set_style_text_font(c, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(c, W_DOT, 0);
        lv_obj_set_pos(c, 140, 11);

        s_row[i]     = row;
        s_row_chk[i] = c;
    }
    s_row_n = n;

    /* 重新扫描后一律清空勾选 */
    s_sel_idx = -1;
    s_sel[0]  = '\0';

    /* 每行约占 1KB 堆 (按钮 + 两个标签), WiFi 那边还吃着 25KB, 打个日志好对账 */
    ESP_LOGI(TAG, "列表 %d 行, 内部堆 %u B", n,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

/* 按 SSID 恢复选中态 (从密码页返回、列表刚重建完时用) */
static void list_restore_sel(const char *ssid)
{
    if (!ssid || !ssid[0]) return;
    for (int i = 0; i < s_row_n; i++) {
        if (strcmp(wifi_mgr_ap_ssid(i), ssid) == 0) {
            s_sel_idx = i;
            row_style(i, true);
            snprintf(s_sel, sizeof s_sel, "%s", ssid);
            return;
        }
    }
}

/* ── 初始化占位页 (只放一行字, 给 esp_wifi_init 让出内存) ── */
static void boot_build(void)
{
    s_boot_pg = page_create();
    s_boot_lbl = mk_label(s_boot_pg, "正在初始化 WiFi…", W_MUTED, 0, 150,
                          TFT_HOR_RES, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_text_align(s_boot_lbl, LV_TEXT_ALIGN_CENTER, 0);
}

static void boot_delete(void)
{
    if (s_boot_pg) { lv_obj_del(s_boot_pg); s_boot_pg = NULL; s_boot_lbl = NULL; }
}

/* ── 连接 WiFi 页 ── */

static void back_cb(lv_event_t *e)
{
    (void)e;
    ui_wifi_back();
}

static void scan_cb(lv_event_t *e)
{
    (void)e;
    if (s_busy || !s_session) return;
    power_mgr_notify_activity();

    /* 按需求: 先清空勾选/列表, 再重新扫描 (进页面时不自动扫, 只有这里触发) */
    s_sel_idx = -1;
    s_sel[0]  = '\0';
    rows_clear();
    list_hint("正在搜索附近 WiFi…");
    set_status("扫描中…");
    set_busy(true);
    wifi_mgr_scan();
}

static void conn_cb(lv_event_t *e)
{
    (void)e;
    if (s_busy || !s_session) return;
    power_mgr_notify_activity();

    if (s_sel_idx < 0 || s_sel[0] == '\0') {
        ESP_LOGI(TAG, "未选择网络, 忽略「连接」");
        return;                     /* 不弹提示, 就是没反应 */
    }
    /* 先藏列表再转屏: 旋转会整屏重绘, 列表页 (172x320) 在横屏坐标系里会被裁掉一半.
     * 同时把 5 行 SSID 对象释放掉 (~5KB): 横屏那一页要临时缓冲 + SPI 反射缓冲,
     * 内部堆越空越稳. 返回时会用 wifi_mgr 里现成的扫描结果重建, 不用重扫. */
    if (s_hint) lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    if (s_list_pg) lv_obj_add_flag(s_list_pg, LV_OBJ_FLAG_HIDDEN);
    rows_clear();

    /* 这台机器以前连成功过: 直接用记住的密码, 密码页整个跳过.
     * 少了那一页, 也就不用那块 2KB 横屏中转缓冲 —— 少一个出错的地方. */
    if (wifi_cred_load(s_sel, s_pass, sizeof s_pass)) {
        s_pass_from_nvs = true;
        start_connect();
        return;
    }

    s_pass_from_nvs = false;
    pass_build();
    s_page = WP_PASS;
}

static void list_build(void)
{
    s_list_pg = page_create();

    s_back_btn = lv_btn_create(s_list_pg);
    lv_obj_set_pos(s_back_btn, 0, 0);
    lv_obj_set_size(s_back_btn, W_TITLE_H, W_TITLE_H);
    lv_obj_set_style_bg_opa(s_back_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_back_btn, 0, 0);
    lv_obj_set_style_shadow_width(s_back_btn, 0, 0);
    lv_obj_set_style_radius(s_back_btn, 0, 0);
    lv_obj_add_event_cb(s_back_btn, back_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *bl = lv_label_create(s_back_btn);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(bl, W_TEXT, 0);
    lv_obj_center(bl);

    mk_label(s_list_pg, "连接 WiFi", W_TEXT, W_TITLE_H, 15,
             TFT_HOR_RES - 2 * W_TITLE_H, LV_TEXT_ALIGN_CENTER);
    mk_line(s_list_pg, W_TITLE_H, TFT_HOR_RES);

    s_rows = lv_obj_create(s_list_pg);
    lv_obj_remove_style_all(s_rows);
    lv_obj_set_pos(s_rows, 0, W_TITLE_H + 1);
    lv_obj_set_size(s_rows, TFT_HOR_RES, W_LIST_H);
    lv_obj_set_style_bg_color(s_rows, W_BG, 0);
    lv_obj_set_style_bg_opa(s_rows, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(s_rows, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_rows, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(s_rows, 0, 0);
    lv_obj_set_style_pad_row(s_rows, 0, 0);
    lv_obj_set_scroll_dir(s_rows, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_rows, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_opa(s_rows, LV_OPA_TRANSP, LV_PART_SCROLLBAR);

    s_hint = mk_label(s_list_pg, "", W_MUTED, 8, W_TITLE_H + 60,
                      TFT_HOR_RES - 16, LV_TEXT_ALIGN_CENTER);
    lv_label_set_long_mode(s_hint, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);

    s_status = mk_label(s_list_pg, "", W_MUTED, 6, 250, TFT_HOR_RES - 12, LV_TEXT_ALIGN_CENTER);
    /* 状态行是中文, 必须用全局中文字库 (mk_label 已经设好), 不要覆盖成 montserrat */

    s_btn_scan = mk_btn(s_list_pg, "扫描", 8, W_BTN_Y, 74, 40);
    lv_obj_add_event_cb(s_btn_scan, scan_cb, LV_EVENT_CLICKED, NULL);
    s_btn_conn = mk_btn(s_list_pg, "连接", 90, W_BTN_Y, 74, 40);
    lv_obj_add_event_cb(s_btn_conn, conn_cb, LV_EVENT_CLICKED, NULL);

    list_hint("按「扫描」搜索附近 WiFi");
}

/* ── 密码页 (横屏) ── */

/* 删密码页并恢复竖屏.
 * 用 lv_obj_del_async: 这个函数可能是在"按钮自己的事件里"调用的 (确认/返回),
 * 同步删掉正在处理事件的对象不安全; 先隐藏再异步删, 中间不会闪出旧页. */
static void pass_delete(void)
{
    if (s_pass_pg) {
        lv_obj_add_flag(s_pass_pg, LV_OBJ_FLAG_HIDDEN);
        lv_obj_del_async(s_pass_pg);        /* 键盘是它的子对象, 跟着一起删 */
        s_pass_pg = NULL;
    }
    s_ta = NULL;
    s_kb = NULL;

    /* 恢复竖屏 (只在真的是横屏时动: lv_disp_set_rotation 会整屏重绘) */
    if (lv_disp_get_rotation(NULL) != LV_DISP_ROT_NONE) {
        lv_disp_set_rotation(NULL, LV_DISP_ROT_NONE);
        lv_obj_invalidate(lv_scr_act());
    }
}

/* 建「正在连接」页并把连接请求交给 worker.
 *
 * 手输密码 (pass_confirm) 和用 NVS 里记住的密码 (conn_cb) 两条路都走这里,
 * 因为两件事必须一模一样:
 *   1. 恢复/保持竖屏 —— 连接页是竖屏的, 横屏那套到这一步就结束了;
 *   2. 先把横屏那块 2KB 中转缓冲还回去 —— 连接一开始就要发 DHCP/ARP, 每帧要
 *      ~1.6KB 的 DMA TX 缓冲, 2KB 还占着的话实测 1595B/最大连续块 800B,
 *      第一帧就 ret=-1, 后面永远拿不到 IP.
 * 释放之后不再补申请: 这个流程里横屏页不会再出现 (失败直接退出流程). */
static void start_connect(void)
{
    conn_page_delete();

    s_conn_pg = page_create();
    mk_label(s_conn_pg, "正在连接", W_TEXT, 0, 112, TFT_HOR_RES, LV_TEXT_ALIGN_CENTER);
    lv_obj_t *ssid_lbl = mk_label(s_conn_pg, s_sel, W_MUTED, 10, 140,
                                  TFT_HOR_RES - 20, LV_TEXT_ALIGN_CENTER);
    lv_label_set_long_mode(ssid_lbl, LV_LABEL_LONG_DOT);
    mk_label(s_conn_pg, "请稍候", W_MUTED, 0, 180, TFT_HOR_RES, LV_TEXT_ALIGN_CENTER);
    s_dots = mk_label(s_conn_pg, ".", W_MUTED, 0, 204, TFT_HOR_RES, LV_TEXT_ALIGN_CENTER);
    if (s_pass_from_nvs) {
        /* 没输密码就连, 得让人知道用的是记住的那份, 不是程序跳步了 */
        mk_label(s_conn_pg, "使用已保存的密码", W_MUTED, 0, 232, TFT_HOR_RES, LV_TEXT_ALIGN_CENTER);
    }
    s_page = WP_CONNECTING;

    set_status("");
    set_busy(true);

    ui_core_rot_stage_release();
    ESP_LOGI(TAG, "连网前释放横屏缓冲: DMA 可用 %u B (最大连续块 %u)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));

    wifi_mgr_connect(s_sel, s_pass);
}

static void pass_confirm(void)
{
    if (s_busy || s_page != WP_PASS) return;
    power_mgr_notify_activity();

    const char *pw = s_ta ? lv_textarea_get_text(s_ta) : "";
    snprintf(s_pass, sizeof s_pass, "%s", pw);
    s_pass_from_nvs = false;        /* 手输的, 连上后要覆盖掉旧记录 */
    ESP_LOGI(TAG, "确认密码: 网络 \"%s\", 密码 %u 字符", s_sel, (unsigned)strlen(s_pass));

    /* 需求: 清空这一页 (不做嵌套) → 恢复竖屏 → 显示「正在连接」 */
    pass_delete();
    start_connect();
}

static void confirm_cb(lv_event_t *e)
{
    (void)e;
    pass_confirm();
}

static void kb_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_READY) pass_confirm();
    /* CANCEL (键盘上的收起键): 什么都不做, 别让人误退出 */
}

static void pass_build(void)
{
    /* 进横屏页之前确保中转缓冲在手 (幂等: 已有就直接返回).
     * 通常是进流程时或"连接失败"时已经申请好; 这里再兜一次, 覆盖
     * "密码页 → 电源键返回 → 再进密码页" 这类来回路径. */
    if (!ui_core_rot_stage_acquire()) {
        ESP_LOGW(TAG, "进密码页时申请横屏缓冲失败, 这一页可能花屏");
    }

    ESP_LOGI(TAG, "进密码页前: 内部堆 %u B (最大块 %u), DMA 可用 %u B (最大连续块 %u)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));

    /* 先旋转再建页: page_create 按当前逻辑分辨率取尺寸 (320x172) */
    lv_disp_set_rotation(NULL, LV_DISP_ROT_90);

    s_pass_pg = page_create();

    /* 顶行从左到右: 「请输入密码」+ 输入框 + 「确认」.
     * 整行压到 y=0..28: 键盘要上移到 y=29, 顶行不抬起来会被盖住一截. */
    mk_label(s_pass_pg, "请输入密码", W_TEXT, 6, 6, 82, LV_TEXT_ALIGN_LEFT);

    s_ta = lv_textarea_create(s_pass_pg);
    lv_obj_set_pos(s_ta, 92, 0);
    lv_obj_set_size(s_ta, 132, 28);
    lv_textarea_set_one_line(s_ta, true);
    lv_textarea_set_max_length(s_ta, 64);
    /* 不设 placeholder: 那个占位文字走 montserrat_16, 中文会渲染成方框, 空着就好.
     * 也不开密码模式: 键盘格子太小, 打成小圆点以后根本看不出哪一位输错了. */
    lv_obj_set_style_text_font(s_ta, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_ta, W_TEXT, 0);
    lv_obj_set_style_bg_color(s_ta, W_BG, 0);
    lv_obj_set_style_bg_opa(s_ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_ta, W_BTN_BR, 0);
    lv_obj_set_style_border_width(s_ta, 1, 0);
    lv_obj_set_style_radius(s_ta, 5, 0);
    lv_obj_set_style_pad_left(s_ta, 8, 0);
    lv_obj_set_style_pad_top(s_ta, 4, 0);
    lv_obj_set_style_pad_bottom(s_ta, 4, 0);

    lv_obj_t *ok = mk_btn(s_pass_pg, "确认", 230, 0, 82, 28);
    lv_obj_add_event_cb(ok, confirm_cb, LV_EVENT_CLICKED, NULL);

    s_kb = lv_keyboard_create(s_pass_pg);
    /* 位置按实测微调: (8,42) → (4,37) → (1,32) → (-1,29) → (0,9) → (0,3) → (0,-3) */
    lv_obj_set_pos(s_kb, 0, -3);
    lv_obj_set_size(s_kb, LAN_W - 16, LAN_H - 29 - 13);     /* 304 x 130 */
    lv_keyboard_set_textarea(s_kb, s_ta);
    lv_keyboard_set_mode(s_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_add_event_cb(s_kb, kb_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_kb, kb_cb, LV_EVENT_CANCEL, NULL);
    /* 注意: 键盘上的"回车"(↵) 不给它挂 READY 回调 —— 它紧贴在"删除"下面,
     * 手指一滑就误触确认. 只有右上角的「确认」键和键盘右下角的 OK 才算提交. */

    /* 键盘配色: 透明底 + 浅灰键, 按下更深 */
    lv_obj_set_style_bg_opa(s_kb, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_kb, 0, 0);
    lv_obj_set_style_pad_all(s_kb, 3, 0);
    lv_obj_set_style_pad_row(s_kb, 3, 0);
    lv_obj_set_style_pad_column(s_kb, 3, 0);
    lv_obj_set_style_border_width(s_kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_radius(s_kb, 5, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_kb, W_BTN_BG, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_kb, W_PRESSED, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(s_kb, lv_color_hex(0xD8D8D8), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(s_kb, W_TEXT, LV_PART_ITEMS);
}

/* ── 正在检查更新 ── */

static void check_delete(void)
{
    if (s_check_pg) { lv_obj_del(s_check_pg); s_check_pg = NULL; s_dots = NULL; }
}

static void check_build(void)
{
    check_delete();
    s_check_pg = page_create();
    mk_label(s_check_pg, "正在检查更新", W_TEXT, 0, 128, TFT_HOR_RES, LV_TEXT_ALIGN_CENTER);
    s_dots = mk_label(s_check_pg, ".", W_MUTED, 0, 162, TFT_HOR_RES, LV_TEXT_ALIGN_CENTER);
}

/* ── 版本页: 当前 / 最新 + 「更新」 ── */

static void ver_delete(void)
{
    if (s_ver_pg) {
        lv_obj_del(s_ver_pg);
        s_ver_pg = NULL; s_ver_cur = NULL; s_ver_new = NULL;
    }
}

static void prog_build(void);

static void update_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_busy) return;
    power_mgr_notify_activity();

    const upd_status_t *st = update_status();
    if (st->phase != UPD_READY) return;     /* 状态不对就什么都不做 */

    prog_build();
    s_page = WP_PROG;
    set_busy(true);                         /* 下载/写入期间电源键无效 */
    wifi_mgr_update_start();
}

static void ver_build(void)
{
    const upd_status_t *st = update_status();
    char b[48];

    ver_delete();
    s_ver_pg = page_create();
    mk_label(s_ver_pg, "发现新版本", W_TEXT, 0, 52, TFT_HOR_RES, LV_TEXT_ALIGN_CENTER);

    snprintf(b, sizeof b, "当前 %s", st->cur_ver);
    s_ver_cur = mk_label(s_ver_pg, b, W_MUTED, 0, 88, TFT_HOR_RES, LV_TEXT_ALIGN_CENTER);

    snprintf(b, sizeof b, "最新 %s", st->new_ver);
    s_ver_new = mk_label(s_ver_pg, b, W_TEXT, 0, 112, TFT_HOR_RES, LV_TEXT_ALIGN_CENTER);

    lv_obj_t *btn = mk_btn(s_ver_pg, "更新", 26, 168, 120, 40);
    lv_obj_add_event_cb(btn, update_click_cb, LV_EVENT_CLICKED, NULL);
}

/* ── 进度页 (下载 + 写入) ── */

static void prog_delete(void)
{
    if (s_prog_pg) {
        lv_obj_del(s_prog_pg);
        s_prog_pg = NULL; s_prog_title = NULL; s_prog_pct = NULL; s_prog_sub = NULL;
    }
}

static void prog_build(void)
{
    prog_delete();
    s_prog_pg = page_create();
    s_prog_title = mk_label(s_prog_pg, "正在下载", W_TEXT, 0, 88, TFT_HOR_RES, LV_TEXT_ALIGN_CENTER);

    /* 百分比用 18px 数字字体: 16px 那套里数字宽度不一样, 跳动时会左右抖 */
    s_prog_pct = mk_label(s_prog_pg, "0%", W_TEXT, 0, 118, TFT_HOR_RES, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_text_font(s_prog_pct, &lv_font_montserrat_18, 0);

    s_prog_sub = mk_label(s_prog_pg, "", W_MUTED, 0, 158, TFT_HOR_RES, LV_TEXT_ALIGN_CENTER);
    mk_label(s_prog_pg, "请勿断电", W_MUTED, 0, 196, TFT_HOR_RES, LV_TEXT_ALIGN_CENTER);
}

/* ── 事件处理 ── */

static void conn_page_delete(void)
{
    if (s_conn_pg) { lv_obj_del(s_conn_pg); s_conn_pg = NULL; s_dots = NULL; }
}

static const char *reason_text(int reason)
{
    switch (reason) {
    case 15:  return "密码错误";
    case 201: return "找不到该网络";
    case -1:  return "连接超时";
    case -2:  return "参数设置失败";
    case -3:  return "启动连接失败";
    default:  return "连接失败";
    }
}

/* ── 结果提示页 (竖屏, 到点自动退出流程) ──
 *
 * 连接失败 / 检查更新失败 / 已是最新 / 更新失败 / 更新完成 全走这一页:
 * 标题 + 两行说明 + 底部一句话 + 一个一次性定时器. 到点和按电源键都是 flow_exit().
 * 以前只有"连接失败"一种, 每加一种结果都要复制一遍"删页面 + 停定时器 + 收尾";
 * 现在只有一套, 新增结果只需要在 handle_evt 里换个文案. */

static void notice_timer_cancel(void)
{
    if (s_notice_timer) { lv_timer_del(s_notice_timer); s_notice_timer = NULL; }
}

static void notice_delete(void)
{
    notice_timer_cancel();
    if (s_notice_pg) { lv_obj_del(s_notice_pg); s_notice_pg = NULL; }
}

/* 到点: 和电源键走的是同一个出口 */
static void notice_exit_cb(lv_timer_t *t)
{
    (void)t;
    s_notice_timer = NULL;      /* 一次性定时器由 LVGL 自己回收, 这里只清指针 */
    flow_exit("提示页超时");
}

/* l1 是灰色小字 (SSID/版本), l2 是主文案, l3 是"即将返回"这种尾巴; 传 NULL 就不建 */
static void notice_build(const char *title, const char *l1, const char *l2,
                         const char *l3, int hold_ms)
{
    notice_delete();
    s_notice_pg = page_create();
    mk_label(s_notice_pg, title, W_TEXT, 0, 96, TFT_HOR_RES, LV_TEXT_ALIGN_CENTER);

    if (l1) {
        lv_obj_t *o = mk_label(s_notice_pg, l1, W_MUTED, 10, 128,
                               TFT_HOR_RES - 20, LV_TEXT_ALIGN_CENTER);
        lv_label_set_long_mode(o, LV_LABEL_LONG_DOT);
    }
    if (l2) mk_label(s_notice_pg, l2, W_TEXT, 0, 162, TFT_HOR_RES, LV_TEXT_ALIGN_CENTER);
    if (l3) mk_label(s_notice_pg, l3, W_MUTED, 0, 196, TFT_HOR_RES, LV_TEXT_ALIGN_CENTER);

    s_notice_timer = lv_timer_create(notice_exit_cb, hold_ms, NULL);
    lv_timer_set_repeat_count(s_notice_timer, 1);
}

static void handle_evt(wifi_ev_t ev, int arg)
{
    switch (ev) {
    case WIFI_EV_READY:
        /* 先销毁占位页再建列表页: 此刻内存已经从 WiFi 初始化里还回来一部分,
         * 列表页那 ~10KB 现在花得起 */
        boot_delete();
        /* 横屏中转缓冲 (2KB): 整个流程只在这里申请一次, 由 ui_wifi_destroy 释放一次.
         * 中途只有在"点确认去连接"那一刻会让出去 (pass_confirm, 连接要发包, 见那里的
         * 说明), 释放之后不再补 —— 因为补的那条路必然失败 (池子只剩 ~1.3KB 连续块),
         * 所以连接失败直接退出流程重来, 而不是回列表.
         * 为什么现在申请: 此刻 esp_wifi_init 刚结束、列表页还没建, DMA 池最宽裕;
         * 等列表页建完就只剩几百字节, 再也拼不出 2KB 连续块. */
        if (!ui_core_rot_stage_acquire()) {
            ESP_LOGE(TAG, "横屏中转缓冲申请失败 (DMA 内存紧张), 密码页会花屏");
        }
        if (!s_list_pg) list_build();   /* 幂等: 重复 READY 也不会建两遍 */
        s_page = WP_LIST;
        set_busy(false);
        set_status("点「扫描」搜索附近 WiFi");
        break;

    case WIFI_EV_ERROR:
        s_session = false;
        set_busy(false);
        if (s_boot_lbl) {
            lv_label_set_text(s_boot_lbl, "WiFi 初始化失败\n按电源键返回");
        }
        ESP_LOGE(TAG, "WiFi 初始化失败, 只能退出本页");
        break;

    case WIFI_EV_SCAN_DONE: {
        set_busy(false);
        list_fill();
        char b[40];
        if (arg > 0) snprintf(b, sizeof b, "找到 %d 个网络", arg);
        else         snprintf(b, sizeof b, "没搜到网络, 可再扫一次");
        set_status(b);
        break;
    }

    case WIFI_EV_SCAN_FAIL:
        set_busy(false);
        rows_clear();
        list_hint("扫描失败, 请重试");
        set_status("扫描失败");
        break;

    case WIFI_EV_CONNECTED:
        set_busy(false);
        conn_page_delete();
        /* 连上了才记: 密码错的那次不会走到这里, 所以不会把错的密码存进去.
         * 手输的和用旧记录连的都会覆盖一次, 保证 NVS 里是最新能用的那份. */
        wifi_cred_save(s_sel, s_pass);
        s_pass_from_nvs = false;
        check_build();
        s_page = WP_CHECK;
        /* 查询期间又要禁止退出: 这一步最长 5 秒 (esp_http_client 的连接超时).
         * 不设的话, 用户能在查询中途退出, 而 worker 还在等 TLS 握手. */
        set_busy(true);
        wifi_mgr_update_check();
        ESP_LOGI(TAG, "已连上 \"%s\", 开始检查更新", s_sel);
        break;

    case WIFI_EV_CONNECT_FAIL:
        /* 不再退回列表: 那会要求在流程中途重新抢 2KB 横屏缓冲 (此时池子只剩
         * ~1.3KB 连续块, 必然失败 -> 再进密码页必花屏). 改成显示 2 秒原因后
         * 退出整个流程, 下次重试就是一次全新的入口. 详见文件头的说明. */
        conn_page_delete();

        /* 只有"密码错"才认定记住的密码作废. 找不到 AP / 超时 都跟密码无关,
         * 删了反而让人下次还得重输. */
        s_cred_dropped = false;
        if (s_pass_from_nvs) {
            if (arg == REASON_WRONG_PASS) {
                ESP_LOGW(TAG, "记住的密码连不上 (密码错误), 删除这条记录");
                s_cred_dropped = wifi_cred_remove(s_sel);
            } else {
                ESP_LOGW(TAG, "记忆密码连接失败 (reason=%d, 不是密码问题), 记录保留", arg);
            }
            s_pass_from_nvs = false;
        }

        s_page = WP_NOTICE;
        set_busy(false);            /* 这 2 秒里电源键可以立刻走 */
        notice_build("连接失败", s_sel,
                     s_cred_dropped ? "保存的密码已失效" : reason_text(arg),
                     s_cred_dropped ? "已清除这次记住的密码" : "即将返回",
                     NOTICE_HOLD_MS);
        ESP_LOGW(TAG, "连接失败 (%s), %d 秒后自动退出流程",
                 s_cred_dropped ? "保存的密码已失效" : reason_text(arg), NOTICE_HOLD_MS / 1000);
        break;

    case WIFI_EV_UPDATE_CHECKED: {
        /* arg = upd_err_t; 0 表示查到了, 但"有没有新版本"看 phase */
        set_busy(false);
        check_delete();
        const upd_status_t *st = update_status();

        if (arg != UPD_OK) {
            char http[24] = "";
            if (st->http) snprintf(http, sizeof http, "HTTP %d", (int)st->http);
            s_page = WP_NOTICE;
            notice_build("检查更新失败", update_err_text(arg), http, "即将返回", NOTICE_HOLD_ERR);
            break;
        }
        if (st->phase == UPD_LATEST) {
            char cur[40];
            snprintf(cur, sizeof cur, "当前 %s", st->cur_ver);
            s_page = WP_NOTICE;
            /* 两行都填: 上面是"当前 0.7.8", 中间是服务器最新版本 */
            notice_build("已是最新版本", cur, st->new_ver, "即将返回", NOTICE_HOLD_MS);
            break;
        }
        ver_build();
        s_page = WP_VER;
        break;
    }

    case WIFI_EV_UPDATE_FAIL: {
        const upd_status_t *st = update_status();
        char http[24] = "";
        if (st->http) snprintf(http, sizeof http, "HTTP %d", (int)st->http);

        prog_delete();
        check_delete();
        s_page = WP_NOTICE;
        set_busy(false);
        notice_build("更新失败", update_err_text(arg), http, "即将返回", NOTICE_HOLD_ERR);
        break;
    }

    case WIFI_EV_UPDATE_DONE:
        /* 页面留着, 1.5 秒后由提示页的定时器走 flow_exit → 释放 WiFi →
         * RELEASED 时直接 esp_restart(). 期间不删页面, 屏幕上不会闪白底. */
        prog_delete();
        s_restart_after_exit = true;
        s_page = WP_NOTICE;
        set_busy(false);
        notice_build("更新完成", update_status()->new_ver, "正在重启", NULL, NOTICE_HOLD_DONE);
        break;

    case WIFI_EV_RELEASED:
        if (s_restart_after_exit) {
            ESP_LOGI(TAG, "新固件已写入, 重启进入校验窗口");
            esp_restart();
        }
        ESP_LOGI(TAG, "WiFi 资源已释放, 退出流程");
        ui_wifi_destroy();
        if (s_end_cb) s_end_cb();
        break;
    }
}

/* 进度页刷新: worker 只写 upd_status_t 里的数字, 这里 5Hz 取一次.
 * 不跨任务碰 LVGL —— 所有 lv_* 调用都还在 LVGL 任务里. */
static void prog_refresh(void)
{
    const upd_status_t *st = update_status();
    uint32_t total = st->total;

    int pct = 0;
    if (total) {
        /* 下载和写入的进度各占一半: dl/wr 都是"已完成字节", 相加再除以 2*total,
         * 这样下载→暂停→写入的循环里进度条匀速上升, 不会来回跳 */
        uint64_t done = (uint64_t)st->dl + st->wr;
        pct = (int)(done * 100 / ((uint64_t)total * 2));
        if (pct > 100) pct = 100;
    }

    if (s_prog_title) {
        lv_label_set_text(s_prog_title, st->phase == UPD_WRITING ? "正在写入" : "正在下载");
    }
    if (s_prog_pct) {
        char b[16];
        snprintf(b, sizeof b, "%d%%", pct);
        lv_label_set_text(s_prog_pct, b);
    }
    if (s_prog_sub) {
        char b[64];
        /* 下载阶段看"已下载", 写入阶段看"已写入" —— 两个计数是分开推进的 */
        uint32_t cur = (st->phase == UPD_WRITING) ? st->wr : st->dl;
        snprintf(b, sizeof b, "%u.%u / %u.%u MB",
                 (unsigned)(cur / (1024u * 1024u)),
                 (unsigned)((cur / (1024u * 102u)) % 10u),
                 (unsigned)(total / (1024u * 1024u)),
                 (unsigned)((total / (1024u * 102u)) % 10u));
        lv_label_set_text(s_prog_sub, b);
    }
}

static void poll_cb(lv_timer_t *t)
{
    (void)t;

    wifi_ev_t ev;
    int       arg = 0;
    while (wifi_mgr_poll(&ev, &arg)) {
        handle_evt(ev, arg);
        if (!s_timer) return;      /* 处理过程中流程结束 (定时器已被删), 不再取事件 */
    }

    /* 「正在…」的点点动画 (连接页和检查更新页共用同一个标签) */
    if ((s_page == WP_CONNECTING || s_page == WP_CHECK) && s_dots) {
        static const char *dots[3] = { ".", "..", "..." };
        s_tick++;
        if ((s_tick % 4) == 0) lv_label_set_text(s_dots, dots[(s_tick / 4) % 3]);
    }

    /* 进度页每 200ms 刷一次 (5Hz): 再快没意义, 文字重绘也不便宜 */
    if (s_page == WP_PROG && (s_prog_tick++ % 2) == 0) {
        prog_refresh();
    }
}

/* ── 唯一出口 ── */

/* 所有"要离开这个流程"的路径都只能走这里: 电源键 / 左上角返回 / 失败页 2 秒超时.
 *
 * 这里只负责"决定要走", 真正的收尾统一由 WIFI_EV_RELEASED -> ui_wifi_destroy() 做,
 * 两条路径共用同一套代码. 以前 end_flow 被三处分别调用, 每加一条退出路径就得记得
 * 补一遍收尾, 漏一处就是资源泄漏或者永远卡在 busy.
 *
 * 幂等: 连点返回 / 定时器到点 / 事件同时到达时只生效第一次. */
static void flow_exit(const char *why)
{
    if (s_page == WP_NONE || s_leaving) return;
    s_leaving = true;

    ESP_LOGI(TAG, "退出流程 (%s): 交给 wifi_mgr 释放, 完成后回设置页", why);

    notice_timer_cancel();      /* 提示页的定时器一并取消, 免得二次触发;
                                 * 页面本身留到 ui_wifi_destroy 再删, 免得释放期间
                                 * 露出底下的列表页闪一下 */
    s_session = false;          /* 不再接受扫描/连接命令 */
    set_busy(true);             /* 释放期间挡住所有退出入口 */
    set_status("正在断开 WiFi…");
    wifi_mgr_release();
}

/* ── 对外接口 ── */

bool ui_wifi_enter(void)
{
    if (s_page != WP_NONE) return false;
    if (wifi_mgr_session_active()) {
        ESP_LOGW(TAG, "上一次 WiFi 会话还没释放完, 暂不进入");
        return false;
    }

    wifi_mgr_flush_events();     /* 丢掉上一次会话的残留事件 */
    s_sel_idx = -1;
    s_sel[0]  = '\0';
    s_pass[0] = '\0';
    s_tick    = 0;
    s_prog_tick = 0;
    s_leaving = false;
    s_pass_from_nvs = false;
    s_cred_dropped  = false;
    s_restart_after_exit = false;
    update_reset();              /* 版本号/进度/错误码全部归零 */

    /* 先只建极简占位页: esp_wifi_init 要 ~28KB DMA 内部堆, 完整列表页要 ~10KB,
     * 先建页会把 init 掐死. 等 WIFI_EV_READY 再换成真正的列表页. */
    boot_build();
    s_page    = WP_BOOT;
    s_session = true;
    set_busy(true);

    if (s_timer) lv_timer_del(s_timer);
    s_timer = lv_timer_create(poll_cb, 100, NULL);

    wifi_mgr_start_session();
    ESP_LOGI(TAG, "进入「连接 WiFi」流程");
    return true;
}

void ui_wifi_back(void)
{
    if (s_page == WP_NONE) return;

    if (s_busy) {
        ESP_LOGI(TAG, "WiFi 操作进行中, 忽略返回");
        return;                 /* 不弹提示, 就是没反应 */
    }

    /* 电源键和左上角返回都走这里; 所有"离开流程"的分支一律调 flow_exit() */
    switch (s_page) {
    case WP_BOOT:
        flow_exit("占位页返回 (初始化失败/尚未就绪)");
        break;

    case WP_LIST:
        flow_exit("列表页返回");
        break;

    case WP_PASS: {
        char keep[33];
        snprintf(keep, sizeof keep, "%s", s_sel);   /* 记住选中项, 回来时恢复 */
        pass_delete();
        if (s_list_pg) {
            lv_obj_clear_flag(s_list_pg, LV_OBJ_FLAG_HIDDEN);
            list_fill();            /* 行对象进密码页时释放了, 按扫描结果重建 */
            list_restore_sel(keep);
        }
        s_page = WP_LIST;
        power_mgr_notify_activity();
        break;
    }

    case WP_CONNECTING:
        ESP_LOGI(TAG, "正在连接, 忽略返回");
        break;

    case WP_CHECK:
        flow_exit("检查更新页返回");
        break;

    case WP_VER:
        /* 查到新版本但用户不想升: 和"检查更新失败"一样整流程退出 */
        flow_exit("版本页返回");
        break;

    case WP_PROG:
        ESP_LOGI(TAG, "下载/写入进行中, 忽略返回");
        break;

    case WP_NOTICE:
        /* 不等那几秒了, 立刻走 —— 和定时器到点是同一个出口 */
        flow_exit("提示页按返回");
        break;

    default:
        break;
    }
}

bool ui_wifi_active(void)
{
    return s_page != WP_NONE;
}

void ui_wifi_destroy(void)
{
    if (s_timer) { lv_timer_del(s_timer); s_timer = NULL; }

    /* 横屏中转缓冲用完就还, 不常驻 (不开 WiFi 的设备上完全不占这 2KB) */
    ui_core_rot_stage_release();

    /* 可能停在横屏的密码页: 先收掉页面并恢复竖屏, 否则后续所有页面都是横的 */
    pass_delete();

    conn_page_delete();
    notice_delete();
    check_delete();
    ver_delete();
    prog_delete();
    boot_delete();

    if (s_list_pg) {
        lv_obj_del(s_list_pg);
        s_list_pg  = NULL;
        s_rows     = NULL;
        s_hint     = NULL;
        s_status   = NULL;
        s_back_btn = NULL;
        s_btn_scan = NULL;
        s_btn_conn = NULL;
    }
    rows_clear();

    if (s_session) {            /* 兜底: 还有会话就让 worker 跑完手上的操作再自己释放 */
        s_session = false;
        wifi_mgr_release();
    }

    s_page    = WP_NONE;
    s_sel_idx = -1;
    s_sel[0]  = '\0';
    s_pass[0] = '\0';
    s_leaving = false;
    s_pass_from_nvs = false;
    s_cred_dropped  = false;
    set_busy(false);            /* 一定要解掉 ui_inhibit, 否则以后永远不熄屏 */

    if (lv_disp_get_rotation(NULL) != LV_DISP_ROT_NONE) {
        lv_disp_set_rotation(NULL, LV_DISP_ROT_NONE);
    }
    lv_obj_invalidate(lv_scr_act());
    ESP_LOGI(TAG, "WiFi 流程页面已清理");
}

void ui_wifi_set_end_cb(void (*cb)(void))
{
    s_end_cb = cb;
}
