#include <string.h>
#include "lvgl.h"
#include "bt_a2dp.h"
#include "menu.h"

/* ── 蓝牙设备列表 ── */
#define BT_W        135    /* 面板宽 */
#define BT_H        220    /* 面板高 */
#define BT_X        13     /* 面板左上角 X */
#define BT_Y        3      /* 面板左上角 Y */
#define BT_ROW_H    36     /* 每行设备高度 */
#define BT_BUF_MAX  16     /* 设备列表缓冲上限 */
#define COLOR_BT_BLUE  lv_color_hex(0x2196F3)   /* 连接中/操作色 */
#define COLOR_BT_RED   lv_color_hex(0xE53935)   /* 断开色 */

/* 蓝牙面板控件句柄 */
static lv_obj_t *s_bt_overlay   = NULL;  /* 全屏透明遮罩 (点外部关闭) */
static lv_obj_t *s_bt_cont      = NULL;  /* 白色面板容器 */
static lv_obj_t *s_bt_title     = NULL;  /* 标题 */
static lv_obj_t *s_bt_list      = NULL;  /* 设备列表 */
static lv_obj_t *s_bt_card      = NULL;  /* 已连/连接中卡片 */
static lv_obj_t *s_bt_card_name = NULL;  /* 卡片设备名 */
static lv_obj_t *s_bt_action_btn  = NULL;  /* 操作按钮 (断开/连接中) */
static lv_obj_t *s_bt_action_lbl  = NULL;  /* 操作按钮文字 */

static bool         s_bt_open = false;         /* 面板是否打开 */
static bool         s_bt_scanning = false;     /* 是否正在扫描 */
static bt_state_t   s_bt_state = BT_STATE_DISCONNECTED;   /* 影子连接状态 */
static char         s_bt_connect_name[32];     /* 当前连接/连接中的设备名 */

static char         s_bt_display[BT_BUF_MAX][32];  /* 当前显示的设备名列表 */
static int          s_bt_display_count = 0;        /* s_bt_display 有效条目数 */
static char         s_bt_pending[BT_BUF_MAX][32];  /* 扫描结果缓冲 (扫描完成一次性刷新) */
static int          s_bt_pending_count = 0;        /* s_bt_pending 已累积条目数 */

static bt_a2dp_iface_t *s_bt_iface = NULL;   /* 蓝牙接口 */

/* 发送蓝牙命令: type=命令, name=目标设备名(可空) */
static void bt_send_cmd(bt_cmd_type_t type, const char *name)
{
    if (!s_bt_iface) return;
    bt_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = type;
    if (name) {
        strncpy(cmd.device_name, name, sizeof(cmd.device_name) - 1);
    }
    xQueueSend(s_bt_iface->cmd_queue, &cmd, 0);
}

static void bt_apply_state(bt_state_t state, const char *name);
static void bt_maybe_start_scan(void);

/* 仅在「列表打开 + 未连接 + 未在扫描」时发一次扫描 */
static void bt_maybe_start_scan(void)
{
    if (!s_bt_open || s_bt_state != BT_STATE_DISCONNECTED || s_bt_scanning) return;
    s_bt_scanning = true;
    bt_send_cmd(BT_CMD_SCAN, NULL);
}

/* 点击某设备行 → 发起连接 */
static void bt_item_click_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    const char *name = (const char *)lv_obj_get_user_data(btn);   /* 设备名存在 user_data */
    if (!name) return;

    s_bt_scanning = false;
    bt_apply_state(BT_STATE_CONNECTING, name);
    bt_send_cmd(BT_CMD_CONNECT, name);
}

/* 操作按钮: 已连接 → 断开 */
static void bt_action_click_cb(lv_event_t *e)
{
    if (s_bt_state == BT_STATE_CONNECTED) {
        bt_send_cmd(BT_CMD_DISCONNECT, NULL);
    }
}

/* 切到"列表视图": 显示设备列表, 隐藏卡片 */
static void bt_show_list_view(void)
{
    lv_obj_clear_flag(s_bt_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_bt_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_bt_action_btn, LV_OBJ_FLAG_HIDDEN);
}

/* 依据给定状态更新影子状态 + (列表打开时)渲染对应视图.
 * state=新状态, name=相关设备名 */
static void bt_apply_state(bt_state_t state, const char *name)
{
    s_bt_state = state;
    if (name && name[0]) {
        strncpy(s_bt_connect_name, name, sizeof(s_bt_connect_name) - 1);
        s_bt_connect_name[sizeof(s_bt_connect_name) - 1] = '\0';
    }

    if (!s_bt_open) return;   /* 面板没开就不渲染 */

    s_bt_scanning = false;

    switch (state) {
    case BT_STATE_DISCONNECTED:   /* 未连接: 回列表, 重新扫描 */
        bt_show_list_view();
        bt_maybe_start_scan();
        break;
    case BT_STATE_CONNECTING:   /* 连接中: 蓝色卡片 */
        lv_label_set_text(s_bt_card_name, s_bt_connect_name);
        lv_obj_set_style_bg_color(s_bt_action_btn, COLOR_BT_BLUE, 0);
        lv_label_set_text(s_bt_action_lbl, "Connecting");
        lv_obj_clear_flag(s_bt_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_bt_action_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_bt_list, LV_OBJ_FLAG_HIDDEN);
        break;
    case BT_STATE_CONNECTED:   /* 已连接: 蓝色卡片 + 红色断开按钮 */
        lv_label_set_text(s_bt_card_name, s_bt_connect_name);
        lv_obj_set_style_bg_color(s_bt_action_btn, COLOR_BT_RED, 0);
        lv_label_set_text(s_bt_action_lbl, "Disconnect");
        lv_obj_clear_flag(s_bt_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_bt_action_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_bt_list, LV_OBJ_FLAG_HIDDEN);
        break;
    default:
        break;
    }
}

/* 重建设备列表 (清空重建, 每行一个按钮) */
static void bt_refresh_list(void)
{
    lv_obj_clean(s_bt_list);

    if (s_bt_display_count == 0) {   /* 空: 显示提示 */
        lv_obj_t *btn = lv_list_add_btn(s_bt_list, NULL, "No devices");
        lv_obj_set_height(btn, BT_ROW_H);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_set_style_pad_top(btn, 7, 0);
        lv_obj_t *label = lv_obj_get_child(btn, 0);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(label, lv_color_black(), 0);
        return;
    }

    for (int i = 0; i < s_bt_display_count; i++) {   /* 每个设备一行 */
        lv_obj_t *btn = lv_list_add_btn(s_bt_list, NULL, s_bt_display[i]);
        lv_obj_set_height(btn, BT_ROW_H);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_set_style_pad_left(btn, 8, 0);
        lv_obj_set_style_pad_top(btn, 7, 0);

        lv_obj_t *label = lv_obj_get_child(btn, 0);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(label, lv_color_black(), 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);

        lv_obj_set_user_data(btn, s_bt_display[i]);   /* 存设备名供点击回调 */
        lv_obj_add_event_cb(btn, bt_item_click_cb, LV_EVENT_CLICKED, NULL);
    }
}

/* 遮罩点击 → 关闭面板 (复用入口回调) */
static void bt_overlay_click_cb(lv_event_t *e)
{
    bt_menu_click_cb(e);
}

/* 实际打开面板: 创建所有控件 */
static void bt_list_open(void)
{
    if (s_bt_open) return;

    s_bt_open = true;
    s_bt_pending_count = 0;
    s_bt_display_count = 0;

    /* 全屏透明遮罩按钮 */
    s_bt_overlay = lv_btn_create(lv_scr_act());
    lv_obj_set_size(s_bt_overlay, 172, 320);
    lv_obj_set_pos(s_bt_overlay, 0, 0);
    lv_obj_set_style_bg_opa(s_bt_overlay, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_bt_overlay, 0, 0);
    lv_obj_set_style_shadow_width(s_bt_overlay, 0, 0);
    lv_obj_add_event_cb(s_bt_overlay, bt_overlay_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_clear_flag(s_bt_overlay, LV_OBJ_FLAG_SCROLLABLE);

    /* 白色面板容器 */
    s_bt_cont = lv_obj_create(s_bt_overlay);
    lv_obj_set_pos(s_bt_cont, BT_X, BT_Y);
    lv_obj_set_size(s_bt_cont, BT_W, BT_H);
    lv_obj_set_style_bg_color(s_bt_cont, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_bt_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_bt_cont, 6, 0);
    lv_obj_set_style_border_color(s_bt_cont, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_border_width(s_bt_cont, 1, 0);
    lv_obj_set_style_pad_all(s_bt_cont, 0, 0);
    lv_obj_clear_flag(s_bt_cont, LV_OBJ_FLAG_SCROLLABLE);

    /* 标题 */
    s_bt_title = lv_label_create(s_bt_cont);
    lv_obj_set_pos(s_bt_title, 6, 4);
    lv_obj_set_size(s_bt_title, BT_W - 12, 22);
    lv_obj_set_style_text_font(s_bt_title, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_bt_title, lv_color_black(), 0);
    lv_label_set_long_mode(s_bt_title, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_bt_title, "Bluetooth");

    /* 设备列表 */
    s_bt_list = lv_list_create(s_bt_cont);
    lv_obj_set_pos(s_bt_list, 0, 28);
    lv_obj_set_size(s_bt_list, BT_W, BT_H - 28);
    lv_obj_set_style_bg_color(s_bt_list, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_bt_list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_bt_list, 0, 0);
    lv_obj_set_style_pad_all(s_bt_list, 0, 0);
    lv_obj_set_style_pad_top(s_bt_list, 8, 0);

    /* 连接状态卡片 */
    s_bt_card = lv_obj_create(s_bt_cont);
    lv_obj_set_pos(s_bt_card, 12, 40);
    lv_obj_set_size(s_bt_card, BT_W - 24, 72);
    lv_obj_set_style_bg_color(s_bt_card, COLOR_BT_BLUE, 0);
    lv_obj_set_style_bg_opa(s_bt_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_bt_card, 12, 0);
    lv_obj_set_style_border_width(s_bt_card, 0, 0);
    lv_obj_set_style_pad_all(s_bt_card, 8, 0);
    lv_obj_clear_flag(s_bt_card, LV_OBJ_FLAG_SCROLLABLE);

    s_bt_card_name = lv_label_create(s_bt_card);
    lv_obj_set_style_text_font(s_bt_card_name, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_bt_card_name, lv_color_white(), 0);
    lv_label_set_long_mode(s_bt_card_name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_bt_card_name, BT_W - 40);
    lv_label_set_text(s_bt_card_name, "");
    lv_obj_align(s_bt_card_name, LV_ALIGN_CENTER, 0, 0);

    /* 底部操作按钮 (连接中/断开) */
    s_bt_action_btn = lv_btn_create(s_bt_cont);
    lv_obj_set_pos(s_bt_action_btn, 12, BT_H - 48);
    lv_obj_set_size(s_bt_action_btn, BT_W - 24, 36);
    lv_obj_set_style_radius(s_bt_action_btn, 10, 0);
    lv_obj_set_style_bg_color(s_bt_action_btn, COLOR_BT_BLUE, 0);
    lv_obj_set_style_bg_opa(s_bt_action_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_bt_action_btn, 0, 0);
    lv_obj_set_style_shadow_width(s_bt_action_btn, 0, 0);
    lv_obj_add_event_cb(s_bt_action_btn, bt_action_click_cb, LV_EVENT_CLICKED, NULL);

    s_bt_action_lbl = lv_label_create(s_bt_action_btn);
    lv_label_set_text(s_bt_action_lbl, "Connecting");
    lv_obj_set_style_text_font(s_bt_action_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_bt_action_lbl, lv_color_white(), 0);
    lv_obj_center(s_bt_action_lbl);

    /* 初始隐藏卡片/按钮, 显示列表 */
    lv_obj_add_flag(s_bt_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_bt_action_btn, LV_OBJ_FLAG_HIDDEN);

    bt_apply_state(s_bt_state, NULL);
    bt_send_cmd(BT_CMD_GET_STATE, NULL);   /* 拉取最新状态 */
}

/* 实际关闭面板: 停止扫描 + 删除全部控件 */
static void bt_list_close(void)
{
    if (!s_bt_open) return;

    if (s_bt_scanning) {   /* 若在扫描则先停止 */
        bt_send_cmd(BT_CMD_STOP_SCAN, NULL);
    }

    s_bt_scanning = false;
    s_bt_open = false;

    if (s_bt_overlay) {
        lv_obj_del(s_bt_overlay);   /* 删除遮罩会连带删除全部子控件 */
    }
    s_bt_overlay   = NULL;
    s_bt_cont      = NULL;
    s_bt_title     = NULL;
    s_bt_list      = NULL;
    s_bt_card      = NULL;
    s_bt_card_name = NULL;
    s_bt_action_btn  = NULL;
    s_bt_action_lbl  = NULL;
}

/* 初始化蓝牙列表: 保存接口 */
void bt_list_init(bt_a2dp_iface_t *iface)
{
    s_bt_iface = iface;
}

/* 蓝牙菜单入口点击: 开/关面板 (带开合动画) */
void bt_menu_click_cb(lv_event_t *e)
{
    if (s_bt_open) {
        static const panel_anim_cfg_t cfg = {
            .overlay_ref = &s_bt_overlay,
            .cont_ref    = &s_bt_cont,
            .w = BT_W, .h = BT_H,
            .x = ANIM_BT_X, .y = ANIM_BT_Y,
            .anchor_right = true,
            .open_real  = bt_list_open,
            .close_real = bt_list_close,
        };
        panel_anim_close(&cfg);
    } else {
        static const panel_anim_cfg_t cfg = {
            .overlay_ref = &s_bt_overlay,
            .cont_ref    = &s_bt_cont,
            .w = BT_W, .h = BT_H,
            .x = ANIM_BT_X, .y = ANIM_BT_Y,
            .anchor_right = true,
            .open_real  = bt_list_open,
            .close_real = bt_list_close,
        };
        panel_anim_open(&cfg);
    }
}

/* 扫描到设备 (面板开着才收, 先存 pending 缓冲) */
void bt_list_on_device_found(const char *name)
{
    if (!s_bt_open) return;
    if (s_bt_pending_count >= BT_BUF_MAX) return;
    strncpy(s_bt_pending[s_bt_pending_count], name, sizeof(s_bt_pending[0]) - 1);
    s_bt_pending[s_bt_pending_count][sizeof(s_bt_pending[0]) - 1] = '\0';
    s_bt_pending_count++;
}

/* 扫描完成: pending → display, 重建列表 */
void bt_list_on_scan_done(void)
{
    if (!s_bt_open) return;
    s_bt_scanning = false;

    s_bt_display_count = s_bt_pending_count;   /* 拷贝结果 */
    for (int i = 0; i < s_bt_pending_count; i++) {
        size_t plen = strnlen(s_bt_pending[i], sizeof(s_bt_display[0]) - 1);
        memcpy(s_bt_display[i], s_bt_pending[i], plen);
        s_bt_display[i][plen] = '\0';
    }
    s_bt_pending_count = 0;

    bt_refresh_list();

    bt_maybe_start_scan();   /* 列表开着且未连接时继续下一轮扫描 (无"列表空"判断) */
}

/* BT 任务回调: A2DP 连接成功 */
void bt_list_on_connected(const char *name)
{
    bt_apply_state(BT_STATE_CONNECTED, name);
}

/* BT 任务回调: 连接失败 → 回到未连接 */
void bt_list_on_connect_failed(const char *name)
{
    (void)name;
    bt_apply_state(BT_STATE_DISCONNECTED, NULL);
}

/* BT 任务回调: 链路断开 → 清连接名并回到未连接 */
void bt_list_on_disconnected(void)
{
    memset(s_bt_connect_name, 0, sizeof(s_bt_connect_name));
    bt_apply_state(BT_STATE_DISCONNECTED, NULL);
}

/* BT 任务回调: 查询状态应答 → 应用到影子状态 */
void bt_list_on_state_rsp(bt_state_t state, const char *name)
{
    bt_apply_state(state, name);
}
