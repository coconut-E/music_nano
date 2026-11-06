/* OTA: 新固件首次启动的"校验窗口" + 槽位诊断 + 命令行切槽
 *
 * 为什么不自己记"这是第一次启动"
 * ------------------------------
 * otadata 里的 ota_state 就是这件事的权威记录:
 *   升级时 esp_ota_set_boot_partition() 写 NEW
 *   下次启动 bootloader 改成 PENDING_VERIFY 然后才装载
 * 所以运行中读到 PENDING_VERIFY ⇔ 这个镜像来自 OTA 且还没被确认。
 * 只要 20 秒内没打标, 任何一次重启都会让 bootloader 把它判成 ABORTED
 * 并退回另一个槽 —— 这就是"启动失败自动退回", 不需要我们写代码。
 *
 * 校验窗口里做什么
 * ----------------
 * 盖一层全屏遮罩 (吃掉所有点击) + 15 秒倒计时, 同时禁止睡眠 (电源键短按/
 * 长按/无操作熄屏全部被拦)。倒计时走完且 UI 确实在跑 (本文件的帧计数在涨)
 * 就 esp_ota_mark_app_valid_cancel_rollback() 打标。
 *
 * 如果用户在窗口内拔电池: 新固件没来得及打标, 下次启动 bootloader 退回旧槽。
 * 这是有意的取舍 —— 硬件上无法区分"用户断电"和"固件挂死后断电", 与其做
 * 重试状态机, 不如在屏幕上明确要求"请勿断电"。
 */

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "lvgl.h"

#include "ota.h"
#include "font_part.h"
#include "power_mgr.h"
#include "ui_core.h"

#define TAG "OTA"

/* 全局中文字库 (resources/lv_font_global_16.c), 工程惯例是在用到的地方 extern */
extern const lv_font_t lv_font_global_16;

#define OTA_VERIFY_SEC     15     /* 校验窗口秒数 */
#define OTA_OVERLAY_WAIT   30     /* 帧计数达到这个值再建遮罩 (让首屏先画完) */
#define OTA_SLOT_NUM       2      /* 分区表里 ota_0 / ota_1 */

/* ---------------------------------------------------------------- */
/* 状态                                                              */
/* ---------------------------------------------------------------- */
static bool           s_verifying  = false;   /* 处于校验窗口 */
static bool           s_never_mark = false;   /* 已尝试过打标 */
static lv_obj_t      *s_overlay    = NULL;
static lv_obj_t      *s_count_lbl  = NULL;
static lv_timer_t    *s_sec_timer  = NULL;
static esp_timer_handle_t s_reboot_timer = NULL;
static int            s_left       = 0;
static volatile uint32_t s_ticks   = 0;       /* LVGL 帧计数 */

/* ---------------------------------------------------------------- */
/* 小工具                                                            */
/* ---------------------------------------------------------------- */
static const char *state_name(esp_ota_img_states_t st)
{
    switch (st) {
    case ESP_OTA_IMG_NEW:            return "NEW";
    case ESP_OTA_IMG_PENDING_VERIFY: return "PENDING_VERIFY";
    case ESP_OTA_IMG_VALID:          return "VALID";
    case ESP_OTA_IMG_INVALID:        return "INVALID";
    case ESP_OTA_IMG_ABORTED:        return "ABORTED";
    default:                         return "UNDEFINED";
    }
}

static const char *reset_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "上电";
    case ESP_RST_EXT:       return "复位脚";
    case ESP_RST_SW:        return "软件重启";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "中断看门狗";
    case ESP_RST_TASK_WDT:  return "任务看门狗";
    case ESP_RST_WDT:       return "看门狗";
    case ESP_RST_DEEPSLEEP: return "深睡唤醒";
    case ESP_RST_BROWNOUT:  return "掉压";
    default:                return "其它";
    }
}

static esp_ota_img_states_t slot_state(const esp_partition_t *p)
{
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (p && esp_ota_get_state_partition(p, &st) != ESP_OK) {
        st = ESP_OTA_IMG_UNDEFINED;
    }
    return st;
}

/* 取某个分区里镜像的版本号 (镜像不存在/无描述符时返回 "-") */
static void slot_version(const esp_partition_t *p, char *out, size_t n)
{
    out[0] = '\0';
    esp_app_desc_t d;
    if (p && esp_ota_get_partition_description(p, &d) == ESP_OK) {
        snprintf(out, n, "%.32s", d.version);
    }
    if (!out[0]) snprintf(out, n, "-");
}

static const esp_partition_t *slot_part(int i)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                    (esp_partition_subtype_t)(ESP_PARTITION_SUBTYPE_APP_OTA_MIN + i),
                                    NULL);
}

/* ---------------------------------------------------------------- */
/* 校验窗口                                                          */
/* ---------------------------------------------------------------- */
static void verify_finish(void)
{
    if (s_sec_timer) { lv_timer_del(s_sec_timer); s_sec_timer = NULL; }
    if (s_overlay)   { lv_obj_del(s_overlay);     s_overlay   = NULL; s_count_lbl = NULL; }

    if (!s_never_mark) {
        s_never_mark = true;
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "校验通过, 已标记 VALID");
        } else {
            ESP_LOGE(TAG, "标记 VALID 失败: %s", esp_err_to_name(err));
        }
    }

    s_verifying = false;
    power_mgr_set_sleep_inhibit(false);
    power_mgr_notify_activity();   /* 刚解锁, 熄屏计时重新开始 */
    ESP_LOGI(TAG, "校验窗口结束");
}

static void verify_sec_cb(lv_timer_t *t)
{
    (void)t;
    if (s_left > 0) s_left--;
    if (s_left <= 0) {
        verify_finish();
        return;
    }
    if (s_count_lbl) {
        char b[16];
        snprintf(b, sizeof b, "%d", s_left);
        lv_label_set_text(s_count_lbl, b);
    }
}

/* 建遮罩: 只能在 LVGL 任务里调用 */
static void verify_build_overlay(void)
{
    /* 必须是"系统层" lv_layer_sys(), 不能用 lv_layer_top():
     *   LVGL 8 的绘制/触摸顺序是 scr -> top -> sys, 触摸搜索反过来先 sys。
     *   本工程的界面切换动画 (mode_anim / menu_anim / ui_player) 都在
     *   lv_layer_top 上放全屏快照图, 而且 mode_anim 会把 lv_scr_act() 整屏
     *   HIDDEN 掉。遮罩若也在 top 层, 窗口期内任何后建的动画图层都会盖在
     *   它上面; 放 sys 层则永远在最上, 且跨 lv_scr_load 存活。 */
    s_overlay = lv_obj_create(lv_layer_sys());
    lv_obj_set_size(s_overlay, TFT_HOR_RES, TFT_VER_RES);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_radius(s_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_overlay, 0, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    /* 默认带 CLICKABLE 且没挂事件: 全屏吃掉触摸 (sys 层第一个被搜到),
     * 校验期间不给用户操作 */

    lv_obj_t *title = lv_label_create(s_overlay);
    lv_label_set_text(title, "新固件校验中");
    lv_obj_set_style_text_font(title, &lv_font_global_16, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 96);

    s_count_lbl = lv_label_create(s_overlay);
    char b[16];
    snprintf(b, sizeof b, "%d", s_left);
    lv_label_set_text(s_count_lbl, b);
    lv_obj_set_style_text_font(s_count_lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_count_lbl, lv_color_white(), 0);
    lv_obj_align(s_count_lbl, LV_ALIGN_TOP_MID, 0, 132);

    lv_obj_t *hint = lv_label_create(s_overlay);
    lv_label_set_text(hint, "请勿断电");
    lv_obj_set_style_text_font(hint, &lv_font_global_16, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0xB0B0B0), 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 190);

    s_sec_timer = lv_timer_create(verify_sec_cb, 1000, NULL);
    ESP_LOGW(TAG, "开始 %d 秒校验窗口 (禁止睡眠)", OTA_VERIFY_SEC);
}

/* ---------------------------------------------------------------- */
/* 对外接口                                                          */
/* ---------------------------------------------------------------- */
static void reboot_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

void ota_init(void)
{
    /* 延迟重启用的定时器 (切槽后 1 秒重启, 让串口日志吐完) */
    if (!s_reboot_timer) {
        const esp_timer_create_args_t args = {
            .callback = reboot_cb,
            .name     = "ota_reboot",
        };
        esp_timer_create(&args, &s_reboot_timer);
    }

    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t   st  = slot_state(run);
    esp_reset_reason_t     rr  = esp_reset_reason();

    char ver[40];
    slot_version(run, ver, sizeof ver);

    printf("\n[OTA] 运行 %s @0x%05lx  v%s  state=%s  本次复位=%s(%d)  字库=%d(%s)\n",
           run ? run->label : "?", run ? (unsigned long)run->address : 0UL,
           ver, state_name(st), reset_name(rr), (int)rr,
           font_part_state(), font_part_state() == FONT_PART_OK ? "ok" : font_part_err());

    if (st != ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "不是首次启动的新固件 (state=%s), 无需校验", state_name(st));
        return;
    }

    /* 只有"刚升级上来还没确认"才进校验窗口 */
    s_verifying  = true;
    s_never_mark = false;
    s_left       = OTA_VERIFY_SEC;
    power_mgr_set_sleep_inhibit(true);   /* 窗口内禁止睡眠 */
    ESP_LOGW(TAG, "检测到新固件首次启动, 进入 %d 秒校验窗口", OTA_VERIFY_SEC);
}

void ota_notify_ui_alive(void)
{
    s_ticks++;
    if (!s_verifying || s_overlay) return;
    if (s_ticks < OTA_OVERLAY_WAIT) return;
    verify_build_overlay();   /* 在 LVGL 上下文里建, 安全 */
}

bool ota_verifying(void)
{
    return s_verifying;
}

void ota_report(void)
{
    const esp_partition_t *run  = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);

    printf("\n[OTA] 运行槽: %s @0x%05lx   启动槽: %s   下次升级写: %s\n",
           run  ? run->label  : "?", run ? (unsigned long)run->address : 0UL,
           boot ? boot->label : "?", next ? next->label : "?");

    for (int i = 0; i < OTA_SLOT_NUM; i++) {
        const esp_partition_t *p = slot_part(i);
        if (!p) {
            printf("[OTA] 槽%d: 分区不存在\n", i);
            continue;
        }
        char ver[40];
        slot_version(p, ver, sizeof ver);
        printf("[OTA] 槽%d %-5s @0x%05lx  v%-10s  state=%s%s\n",
               i, p->label, (unsigned long)p->address, ver,
               state_name(slot_state(p)),
               (run && p->address == run->address) ? "   <- 正在跑" : "");
    }
    printf("[OTA] 校验窗口: %s\n\n", s_verifying ? "进行中" : "无");
}

esp_err_t ota_switch(int slot)
{
    if (slot < 0 || slot >= OTA_SLOT_NUM) {
        printf("[OTA] 槽号只能是 0 或 1 (ota_0 / ota_1)\n");
        return ESP_ERR_INVALID_ARG;
    }
    if (s_verifying) {
        printf("[OTA] 校验窗口内不允许切换, 等 %d 秒\n", s_left);
        return ESP_ERR_INVALID_STATE;
    }

    const esp_partition_t *p = slot_part(slot);
    if (!p) {
        printf("[OTA] 分区表里没有 ota_%d\n", slot);
        return ESP_ERR_NOT_FOUND;
    }

    /* 切到自己没有意义, 而且会写出一条指向"正在运行的分区"的 otadata:
     * 一旦新镜像没写成功, 下次启动就找不到可回退的槽。直接拒绝。 */
    if (esp_ota_get_running_partition() == p) {
        printf("[OTA] 已经在跑 %s, 不需要切槽\n", p->label);
        return ESP_ERR_INVALID_STATE;
    }

    /* 这里会校验目标镜像 (magic/段校验和/SHA256): 没烧或烧坏了直接拒绝,
     * 不会写 otadata, 也不会重启。 */
    esp_err_t err = esp_ota_set_boot_partition(p);
    if (err != ESP_OK) {
        printf("[OTA] 切到 %s 失败: %s (镜像无效或未烧写)\n",
               p->label, esp_err_to_name(err));
        return err;
    }

    char ver[40];
    slot_version(p, ver, sizeof ver);
    printf("[OTA] 已写 otadata: 下次从 %s @0x%05lx (v%s) 启动, 1 秒后重启\n",
           p->label, (unsigned long)p->address, ver);
    if (s_reboot_timer) esp_timer_start_once(s_reboot_timer, 1000 * 1000);
    return ESP_OK;
}
