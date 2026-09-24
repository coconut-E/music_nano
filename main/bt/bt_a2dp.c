#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_timer.h"
#include "bt_a2dp.h"
#include "audio_task.h"
#include "atomic_utils.h"
#include "settings.h"

#define BT_TAG              "BT_A2DP"
#define RC_TAG              "BT_RC"
#define LOCAL_DEVICE_NAME   "ESP_MUSIC"   /* 本机蓝牙广播名 */

#define APP_RC_CT_TL_GET_CAPS          (0)   /* AVRCP 事务标签: 查能力 */
#define APP_RC_CT_TL_RN_VOLUME_CHANGE  (1)   /* AVRCP 事务标签: 音量变化通知 */

#define CACHE_MAX 16        /* 扫描缓存设备数上限 */
#define VOL_STEP  4   /* 耳机音量±键一次步进 */

/* 连接后音量设置时序: 等 AVRC 连上 → 再等 VOL_SET_DELAY_MS → 发音量 → 放行 A2DP 流启动.
 * 静音机制已删除: 窗口期流不启动, 到点先发音量再启流.
 * VOL_WAIT_FALLBACK_MS: AVRC 迟迟连不上时的兜底超时, 防止永远不启流 */
#define VOL_SET_DELAY_MS  500      /* 音量发送延迟 */
#define VOL_WAIT_FALLBACK_MS  3000 /* AVRC 兜底超时 */

/* 内部事件分发类型: 区分协议栈回调来源 */
typedef enum {
    BT_DISPATCH_A2DP,     /* A2DP 回调 */
    BT_DISPATCH_AVRC,     /* AVRCP 控制器回调 */
    BT_DISPATCH_AVRC_TG,  /* AVRCP 目标端回调 (耳机发来的命令) */
} bt_dispatch_type_t;

/* 分发消息: 协议栈回调参数原样拷贝后投递到任务上下文处理 */
typedef struct {
    bt_dispatch_type_t type;   /* 回调来源 */
    uint16_t           event;  /* 事件号 */
    void              *param;  /* 参数副本 (malloc, 处理后 free) */
} bt_dispatch_msg_t;

static bt_a2dp_iface_t s_iface;         /* 对外接口 (队列/流) */
static bool s_bt_ready = false;         /* 协议栈是否就绪 */
static bool s_connected = false;        /* A2DP 已连接 */
static bool s_scanning = false;         /* 正在扫描 */
static bool s_connecting = false;       /* 正在连接 */
static bool s_stream_started = false;   /* 媒体流是否已启动 (START/SUSPEND 状态) */
static esp_bd_addr_t s_peer_bda;        /* 对端(耳机)蓝牙地址 */

/* 连接后音量时序: 等 AVRC 连上后延迟 VOL_SET_DELAY_MS 发音量, 期间禁止启动流 */
static volatile bool s_pending_vol = false;  /* 是否处于"待发音量"窗口 */
static bool          s_avrc_connected = false;  /* AVRC 是否已连接 */
static int64_t       s_avrc_at_us  = 0;   /* AVRC 连接时刻 (us) */
static int64_t       s_connect_at_us = 0; /* A2DP 连接时刻 (us) */


/* 扫描结果缓存: 本次扫描发现的设备 (名称→地址) */
static uint8_t s_cache_count = 0;
static esp_bd_addr_t s_cache_bda[CACHE_MAX];
static char s_cache_name[CACHE_MAX][32];

/* 连接目标状态 */
static char        s_connect_target[32];   /* 目标设备名 (非空=在连接流程中) */
static int         s_connect_retry;        /* 已重试次数 */
static bool        s_connect_found;        /* 扫描是否已找到目标 */
static bool        s_pending_connect_scan = false;  /* 需要"连接扫描"(先取消空闲扫描) */
static esp_bd_addr_t s_connect_bda;        /* 目标设备地址 */

static char        s_connecting_name[32];  /* 连接中的设备名 */
static char        s_connected_name[32];   /* 已连接的设备名 */

static esp_avrc_rn_evt_cap_mask_t s_avrc_peer_rn_cap;  /* 耳机端通知能力位图 */

static int32_t s_last_sent_vol = -1; /* 已同步到耳机的音量, -1 表示未同步 */

static QueueHandle_t    s_dispatch_queue = NULL;  /* 协议栈事件分发队列 */
static QueueSetHandle_t s_queue_set      = NULL;  /* 队列集 (命令+分发) */

/* 蓝牙地址 → 可读字符串 "xx:xx:xx:xx:xx:xx". 返回 str 或 NULL */
static char *bda2str(esp_bd_addr_t bda, char *str, size_t size)
{
    if (bda == NULL || str == NULL || size < 18) {
        return NULL;
    }
    sprintf(str, "%02x:%02x:%02x:%02x:%02x:%02x",
            bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
    return str;
}

/* 从扫描的 EIR 数据里解析设备名 (优先完整名, 退而短名) */
static bool get_name_from_eir(uint8_t *eir, char *bdname, size_t max_len)
{
    uint8_t *name = NULL;
    uint8_t name_len = 0;
    if (!eir) return false;

    name = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &name_len);
    if (!name) {
        name = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &name_len);
    }
    if (name) {
        if (name_len >= max_len) name_len = max_len - 1;   /* 截断到缓冲大小 */
        memcpy(bdname, name, name_len);
        bdname[name_len] = '\0';
        return true;
    }
    return false;
}

/* 在扫描缓存中按名字找索引, 未找到返回 -1 */
static int cache_find(const char *device_name)
{
    for (int i = 0; i < s_cache_count; i++) {
        if (strcmp(s_cache_name[i], device_name) == 0) {
            return i;
        }
    }
    return -1;
}

/* 向扫描缓存添加设备 (去重; 满了从头覆盖) */
static bool cache_add(esp_bd_addr_t bda, const char *device_name)
{
    if (cache_find(device_name) >= 0) {
        return false;   /* 已存在 */
    }
    if (s_cache_count >= CACHE_MAX) {
        s_cache_count = 0;   /* 环形覆盖 */
    }
    memcpy(s_cache_bda[s_cache_count], bda, ESP_BD_ADDR_LEN);
    size_t clen = strnlen(device_name, sizeof(s_cache_name[0]) - 1);
    memcpy(s_cache_name[s_cache_count], device_name, clen);
    s_cache_name[s_cache_count][clen] = '\0';
    s_cache_count++;
    return true;
}

/* 发送蓝牙事件给 UI: type=事件类型, name=相关设备名(可空), error=错误码 */
static void send_evt(bt_evt_type_t type, const char *name, int error)
{
    bt_evt_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = type;
    evt.error = error;
    if (name) {
        size_t nlen = strnlen(name, sizeof(evt.device_name) - 1);
        memcpy(evt.device_name, name, nlen);
        evt.device_name[nlen] = '\0';
    }
    xQueueSend(s_iface.evt_queue, &evt, 0);
}

/* 回发当前状态给列表, 用于查询应答与被拒命令的校正 */
static void send_state_rsp(void)
{
    bt_evt_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = BT_EVT_STATE_RSP;
    if (s_connected) {
        evt.state = BT_STATE_CONNECTED;
        size_t clen = strnlen(s_connected_name, sizeof(evt.device_name) - 1);
        memcpy(evt.device_name, s_connected_name, clen);
        evt.device_name[clen] = '\0';
    } else if (s_connecting) {
        evt.state = BT_STATE_CONNECTING;
        size_t clen = strnlen(s_connecting_name, sizeof(evt.device_name) - 1);
        memcpy(evt.device_name, s_connecting_name, clen);
        evt.device_name[clen] = '\0';
    } else {
        evt.state = BT_STATE_DISCONNECTED;
    }
    xQueueSend(s_iface.evt_queue, &evt, 0);
}

/* 把协议栈回调事件投递到内部队列 (回调在 BT 上下文, 必须快速返回).
 * param 按 param_len 拷贝, 由任务上下文处理后 free. */
static bool bt_a2dp_send_dispatch(bt_dispatch_type_t type, uint16_t event, void *param, size_t param_len)
{
    bt_dispatch_msg_t msg;
    msg.type  = type;
    msg.event = event;
    msg.param = malloc(param_len);
    if (!msg.param) return false;
    memcpy(msg.param, param, param_len);
    if (xQueueSend(s_dispatch_queue, &msg, 0) != pdTRUE) {   /* 队列满则丢弃 */
        free(msg.param);
        return false;
    }
    return true;
}

/* ── AVRCP notify handler ── */
/* 耳机支持音量变化通知则注册之, 以便跟踪耳机端音量改动 */
static void bt_a2dp_volume_changed(void)
{
    if (esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST, &s_avrc_peer_rn_cap,
                                           ESP_AVRC_RN_VOLUME_CHANGE)) {
        esp_avrc_ct_send_register_notification_cmd(APP_RC_CT_TL_RN_VOLUME_CHANGE,
                                                    ESP_AVRC_RN_VOLUME_CHANGE, 0);
    }
}

/* AVRCP 通知事件处理: 目前处理耳机端音量变化 */
static void bt_a2dp_notify_evt_handler(uint8_t event_id, esp_avrc_rn_param_t *event_parameter)
{
    switch (event_id) {
    case ESP_AVRC_RN_VOLUME_CHANGE: {
        ESP_LOGI(RC_TAG, "音量已变化: %d", event_parameter->volume);
        /* 耳机端音量已生效, 同步全局并标记已同步, 避免 5Hz 轮询回环 */
        volume_set(event_parameter->volume);
        s_last_sent_vol = event_parameter->volume;
        bt_a2dp_volume_changed();   /* 续约通知 */
        break;
    }
    default:
        break;
    }
}

/* ── AVRCP event handler (控制器端: 与耳机的命令/应答) ── */
static void bt_a2dp_hdl_avrc_evt(uint16_t event, void *p_param)
{
    esp_avrc_ct_cb_param_t *rc = (esp_avrc_ct_cb_param_t *)p_param;

    switch (event) {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT: {   /* AVRC 连接状态变化 */
        uint8_t *bda = rc->conn_stat.remote_bda;
        ESP_LOGI(RC_TAG, "AVRC 连接状态事件: 状态 %d, [%02x:%02x:%02x:%02x:%02x:%02x]",
                 rc->conn_stat.connected, bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
        if (rc->conn_stat.connected) {
            esp_avrc_ct_send_get_rn_capabilities_cmd(APP_RC_CT_TL_GET_CAPS);   /* 查通知能力 */
            /* AVRC 就绪: 记时间戳, 由连接时序延迟 VOL_SET_DELAY_MS 后统一发音量 */
            s_avrc_connected = true;
            s_avrc_at_us = esp_timer_get_time();
        } else {
            s_avrc_connected = false;   /* 断开: 清状态 */
            s_avrc_at_us = 0;
            s_avrc_peer_rn_cap.bits = 0;
            s_last_sent_vol = -1;
        }
        break;
    }
    case ESP_AVRC_CT_PASSTHROUGH_RSP_EVT:   /* passthrough 应答, 无需处理 */
        break;
    case ESP_AVRC_CT_METADATA_RSP_EVT: {   /* 元数据响应 (播放曲目信息) */
        ESP_LOGI(RC_TAG, "AVRC 元数据响应: 属性ID 0x%x, %s", rc->meta_rsp.attr_id, rc->meta_rsp.attr_text);
        free(rc->meta_rsp.attr_text);   /* 协议栈分配, 需释放 */
        break;
    }
    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT: {   /* 通知事件 */
        bt_a2dp_notify_evt_handler(rc->change_ntf.event_id, &rc->change_ntf.event_parameter);
        break;
    }
    case ESP_AVRC_CT_REMOTE_FEATURES_EVT: {   /* 远程功能集 */
        ESP_LOGI(RC_TAG, "AVRC 远程功能 %"PRIx32", 目标功能 %x",
                 rc->rmt_feats.feat_mask, rc->rmt_feats.tg_feat_flag);
        break;
    }
    case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT: {   /* 通知能力应答 */
        s_avrc_peer_rn_cap.bits = rc->get_rn_caps_rsp.evt_set.bits;
        ESP_LOGI(RC_TAG, "远程通知能力: 数量 %d, 位掩码 0x%x",
                 rc->get_rn_caps_rsp.cap_count, rc->get_rn_caps_rsp.evt_set.bits);
        bt_a2dp_volume_changed();
        break;
    }
    case ESP_AVRC_CT_SET_ABSOLUTE_VOLUME_RSP_EVT: {   /* 绝对音量设置应答 */
        ESP_LOGI(RC_TAG, "设置绝对音量响应: %d", rc->set_volume_rsp.volume);
        /* 耳机确认后的实际音量(可能被钳位) */
        volume_set(rc->set_volume_rsp.volume);
        s_last_sent_vol = rc->set_volume_rsp.volume;
        break;
    }
    default:
        break;
    }
}

/* ── AVRCP TG event handler (耳机发来的控制命令) ── */
static void bt_a2dp_hdl_avrc_tg_evt(uint16_t event, void *p_param)
{
    esp_avrc_tg_cb_param_t *rc = (esp_avrc_tg_cb_param_t *)p_param;

    switch (event) {
    case ESP_AVRC_TG_CONNECTION_STATE_EVT:
        ESP_LOGI(RC_TAG, "AVRC TG 连接状态: %d", rc->conn_stat.connected);
        break;
    case ESP_AVRC_TG_REMOTE_FEATURES_EVT:
        ESP_LOGI(RC_TAG, "AVRC TG 远程功能: 0x%" PRIx32, rc->rmt_feats.feat_mask);
        break;
    case ESP_AVRC_TG_PASSTHROUGH_CMD_EVT:   /* 耳机按键透传命令 */
        ESP_LOGI(RC_TAG, "AVRC passthrough cmd: key 0x%x state %d",
                 rc->psth_cmd.key_code, rc->psth_cmd.key_state);
        /* 只处理按下, 忽略抬起 (防 PRESS+RELEASE 双触发) */
        if (rc->psth_cmd.key_state != 0) break;
        switch (rc->psth_cmd.key_code) {
        case ESP_AVRC_PT_CMD_PLAY:
        case ESP_AVRC_PT_CMD_PAUSE:   /* 播放/暂停键 */
            ESP_LOGI(RC_TAG, "耳机请求 切换播放/暂停");
            send_evt(BT_EVT_PLAY_PAUSE, NULL, 0);
            break;
        case ESP_AVRC_PT_CMD_FORWARD:   /* 下一曲 */
            ESP_LOGI(RC_TAG, "耳机请求 下一曲");
            send_evt(BT_EVT_TRANSPORT_NEXT, NULL, 0);
            break;
        case ESP_AVRC_PT_CMD_BACKWARD:   /* 上一曲 */
            ESP_LOGI(RC_TAG, "耳机请求 上一曲");
            send_evt(BT_EVT_TRANSPORT_PREV, NULL, 0);
            break;
        case ESP_AVRC_PT_CMD_VOL_UP:   /* 耳机音量+ */
            ESP_LOGI(RC_TAG, "耳机 音量+");
            volume_inc(VOL_STEP);
            break;
        case ESP_AVRC_PT_CMD_VOL_DOWN:   /* 耳机音量- */
            ESP_LOGI(RC_TAG, "耳机 音量-");
            volume_inc(-VOL_STEP);
            break;
        default:
            break;
        }
        break;
    case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT:   /* 耳机设定绝对音量 */
        ESP_LOGI(RC_TAG, "耳机绝对音量: %d", rc->set_abs_vol.volume);
        volume_set(rc->set_abs_vol.volume);
        break;
    default:
        break;
    }
}

/* ── A2DP data callback ── */
static uint32_t s_cb_call_count    = 0;
static uint64_t s_cb_total_bytes   = 0;
static uint64_t s_cb_total_got     = 0;
static int64_t  s_cb_last_print_us = 0;

/* 欠载诊断: pcm_stream 不够取时记录 (已禁用, 调试用) */
#if 0
extern volatile bool g_anim_active;
static int64_t  s_cb_last_underrun_us = 0;
static uint32_t s_cb_underrun_count   = 0;
static uint32_t s_cb_underrun_empty   = 0;
#endif

static int32_t bt_a2dp_data_cb(uint8_t *data, int32_t len)
{
    if (!data || len <= 0) {
        return 0;
    }

    s_cb_call_count++;

    if (!s_connected) {   /* 未连接: 补静音, 防止 A2DP 编码器异常 */
        memset(data, 0, (size_t)len);
        return len;
    }

    /* 从 PCM 流取数据; 不足补静音 (欠载保护) */
    size_t got = xStreamBufferReceive(s_iface.pcm_stream, data, (size_t)len, 0);
    if (got < (size_t)len) {
        memset(data + got, 0, (size_t)len - got);   /* 补零 */
#if 0
        s_cb_underrun_count++;
        int64_t now = esp_timer_get_time();
        if ((now - s_cb_last_underrun_us) >= 50000LL) {
            s_cb_last_underrun_us = now;
            size_t avail = xStreamBufferBytesAvailable(s_iface.pcm_stream);
            if (avail == 0) s_cb_underrun_empty++;
        }
#endif
    }

    s_cb_total_bytes += (uint64_t)len;
    s_cb_total_got   += (uint64_t)got;

    int64_t now = esp_timer_get_time();
    if ((now - s_cb_last_print_us) >= 5000000LL) {
        s_cb_last_print_us = now;
        /* [SPI 测量期间] BT_CB 汇总打印已禁用 */
    }

    return len;
}

/* ── A2DP event handler ── */
static void bt_a2dp_hdl_a2d_evt(uint16_t event, void *p_param)
{
    esp_a2d_cb_param_t *a2d = (esp_a2d_cb_param_t *)p_param;

    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT: {   /* A2DP 连接状态 */
        if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            s_connected = true;
            s_connecting = false;
            s_stream_started = false;
            memcpy(s_peer_bda, a2d->conn_stat.remote_bda, ESP_BD_ADDR_LEN);
            /* 进入音量待设窗口: 等 AVRC 连上后再延迟发音量, 期间不启动流 */
            s_pending_vol = true;
            s_avrc_connected = false;
            s_avrc_at_us = 0;
            s_connect_at_us = esp_timer_get_time();
            strncpy(s_connected_name, s_connecting_name,
                    sizeof(s_connected_name) - 1);   /* 连接中的名字转正 */
            s_connected_name[sizeof(s_connected_name) - 1] = '\0';
            memset(s_connecting_name, 0, sizeof(s_connecting_name));
            ESP_LOGI(BT_TAG, "A2DP 已连接");
            send_evt(BT_EVT_CONNECTED, s_connected_name, 0);
        } else if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            /* 断开: 清所有连接状态 */
            s_connected = false;
            s_connecting = false;
            s_stream_started = false;
            s_pending_vol = false;
            s_avrc_connected = false;
            s_avrc_at_us = 0;
            s_connect_at_us = 0;
            memset(s_connected_name, 0, sizeof(s_connected_name));
            memset(s_connecting_name, 0, sizeof(s_connecting_name));
            ESP_LOGI(BT_TAG, "A2DP 已断开连接");
            send_evt(BT_EVT_DISCONNECTED, NULL, 0);
            send_evt(BT_EVT_STREAM_STOPPED, NULL, 0);
        }
        break;
    }
    case ESP_A2D_MEDIA_CTRL_ACK_EVT: {   /* 媒体控制 ACK */
        if (a2d->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_START &&
            a2d->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
            ESP_LOGI(BT_TAG, "A2DP 流媒体已启动");
            send_evt(BT_EVT_STREAM_READY, NULL, 0);
        } else if (a2d->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_SUSPEND &&
                   a2d->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
            ESP_LOGI(BT_TAG, "A2DP 流媒体已挂起");
            send_evt(BT_EVT_STREAM_STOPPED, NULL, 0);
        }
        break;
    }
    case ESP_A2D_AUDIO_STATE_EVT:   /* 音频状态, 无需处理 */
    case ESP_A2D_AUDIO_CFG_EVT:     /* 音频配置, 无需处理 */
        break;
    case ESP_A2D_REPORT_SNK_DELAY_VALUE_EVT: {   /* 接收端延迟值 (诊断) */
        ESP_LOGI(BT_TAG, "延迟值: %u * 1/10 毫秒", a2d->a2d_report_delay_value_stat.delay_value);
        break;
    }
    default:
        break;
    }
}

/* ── GAP callback (called from BT context, keep it fast) ── */
static void bt_a2dp_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    char bda_str[18];

    switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT: {   /* 扫描发现一个设备 */
        if (!s_scanning) break;

        uint32_t cod = 0;
        uint8_t *eir = NULL;

        /* 提取设备属性 (类别/扩展信息) */
        for (int i = 0; i < param->disc_res.num_prop; i++) {
            esp_bt_gap_dev_prop_t *p = param->disc_res.prop + i;
            switch (p->type) {
            case ESP_BT_GAP_DEV_PROP_COD:   /* 服务类别 */
                cod = *(uint32_t *)(p->val);
                break;
            case ESP_BT_GAP_DEV_PROP_EIR:   /* 扩展信息 (含名字) */
                eir = (uint8_t *)(p->val);
                break;
            default:
                break;
            }
        }

        /* 只收渲染类设备 (音箱/耳机), 过滤掉手机等 */
        if (!esp_bt_gap_is_valid_cod(cod) ||
            !(esp_bt_gap_get_cod_srvc(cod) & ESP_BT_COD_SRVC_RENDERING)) {
            break;
        }

        char device_name[32] = {0};
        if (eir && get_name_from_eir(eir, device_name, sizeof(device_name))) {
            bda2str(param->disc_res.bda, bda_str, sizeof(bda_str));
            ESP_LOGI(BT_TAG, "扫描到设备: %s, 名称 %s", bda_str, device_name);

            if (s_connect_target[0] != '\0') {   /* 连接模式: 匹配目标名 */
                if (strcmp(device_name, s_connect_target) == 0) {
                    s_connect_found = true;
                    memcpy(s_connect_bda, param->disc_res.bda, ESP_BD_ADDR_LEN);
                    esp_bt_gap_cancel_discovery();   /* 找到了, 停止扫描 */
                }
            } else {   /* 浏览模式: 加入缓存并上报 */
                if (cache_add(param->disc_res.bda, device_name)) {
                    send_evt(BT_EVT_DEVICE_FOUND, device_name, 0);
                }
            }
        }
        break;
    }
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT: {   /* 扫描状态变化 */
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            s_scanning = false;

            if (s_connect_target[0] != '\0') {   /* 连接扫描结束 */
                if (s_connect_found) {
                    ESP_LOGI(BT_TAG, "找到目标设备 %s，开始连接", s_connect_target);
                    memcpy(s_peer_bda, s_connect_bda, ESP_BD_ADDR_LEN);
                    esp_a2d_source_connect(s_peer_bda);   /* 发起 A2DP 连接 */
                    memset(s_connect_target, 0, sizeof(s_connect_target));
                } else if (s_pending_connect_scan) {   /* 之前被空闲扫描占用, 现在重启连接扫描 */
                    s_pending_connect_scan = false;
                    ESP_LOGI(BT_TAG, "空闲扫描已取消，开始连接扫描: %s", s_connect_target);
                    s_scanning = true;
                    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY,
                                               1, 0);
                } else if (s_connect_retry < 4) {   /* 重试扫描 (最多 4 次) */
                    s_connect_retry++;
                    ESP_LOGI(BT_TAG, "未找到设备 %s，第 %d 次重试",
                             s_connect_target, s_connect_retry);
                    s_scanning = true;
                    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY,
                                               3, 0);
                } else {   /* 彻底失败 */
                    ESP_LOGW(BT_TAG, "未找到设备 %s，连接失败", s_connect_target);
                    s_connecting = false;
                    memset(s_connecting_name, 0, sizeof(s_connecting_name));
                    send_evt(BT_EVT_CONNECT_FAILED, s_connect_target, -1);
                    memset(s_connect_target, 0, sizeof(s_connect_target));
                }
            } else {   /* 普通扫描结束 */
                ESP_LOGI(BT_TAG, "设备搜索已停止。");
                send_evt(BT_EVT_SCAN_DONE, NULL, 0);
            }
        } else if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED) {
            s_scanning = true;
            ESP_LOGI(BT_TAG, "设备搜索已开始。");
        }
        break;
    }
    case ESP_BT_GAP_AUTH_CMPL_EVT: {   /* 认证完成 */
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(BT_TAG, "认证成功: %s", param->auth_cmpl.device_name);
        } else {
            ESP_LOGE(BT_TAG, "认证失败, 状态: %d", param->auth_cmpl.stat);
        }
        break;
    }
    case ESP_BT_GAP_PIN_REQ_EVT: {   /* 旧式 PIN 配对请求: 自动应答 */
        if (param->pin_req.min_16_digit) {   /* 16 位 PIN */
            esp_bt_pin_code_t pin_code = {0};
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 16, pin_code);
        } else {   /* 4 位 PIN "1234" */
            esp_bt_pin_code_t pin_code;
            pin_code[0] = '1';
            pin_code[1] = '2';
            pin_code[2] = '3';
            pin_code[3] = '4';
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin_code);
        }
        break;
    }
    case ESP_BT_GAP_CFM_REQ_EVT:   /* 数字确认配对: 直接确认 */
        ESP_LOGI(BT_TAG, "请比对数字: %06"PRIu32, param->cfm_req.num_val);
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    case ESP_BT_GAP_KEY_NOTIF_EVT:   /* 密钥显示 */
        ESP_LOGI(BT_TAG, "配对密钥: %06"PRIu32, param->key_notif.passkey);
        break;
    case ESP_BT_GAP_KEY_REQ_EVT:   /* 要求输入密钥 */
        ESP_LOGI(BT_TAG, "请输入配对密钥!");
        break;
    default:
        break;
    }
}

/* ── A2DP callback -> dispatch to internal queue ── */
static void bt_a2dp_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    bt_a2dp_send_dispatch(BT_DISPATCH_A2DP, event, param, sizeof(esp_a2d_cb_param_t));
}

/* ── AVRCP callback -> dispatch to internal queue ── */
static void bt_a2dp_rc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    bt_a2dp_send_dispatch(BT_DISPATCH_AVRC, event, param, sizeof(esp_avrc_ct_cb_param_t));
}

/* ── AVRCP TG callback -> dispatch to internal queue ── */
static void bt_a2dp_rc_tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param)
{
    bt_a2dp_send_dispatch(BT_DISPATCH_AVRC_TG, event, param, sizeof(esp_avrc_tg_cb_param_t));
}

/* ── Stack up handler ── */
/* 注册所有协议栈回调并配置功能 (在 BT 任务启动后调用一次) */
static void bt_a2dp_hdl_stack_up(void)
{
    char *dev_name = LOCAL_DEVICE_NAME;
    esp_bt_gap_set_device_name(dev_name);                /* 设置本机名 */
    esp_bt_gap_register_callback(bt_a2dp_gap_cb);        /* GAP 回调 (配对/扫描) */

    esp_avrc_ct_init();                                  /* AVRCP 控制器 (控制耳机) */
    esp_avrc_ct_register_callback(bt_a2dp_rc_ct_cb);

    esp_avrc_tg_init();                                  /* AVRCP 目标端 (接收耳机按键) */
    esp_avrc_tg_register_callback(bt_a2dp_rc_tg_cb);

    /* 关键: TG 默认支持的 passthrough 命令集是全 0, 耳机发的 PLAY/PAUSE/上下曲
     * 全被协议栈回 NOT_IMPL 且不产生回调, 必须显式开启 */
    esp_avrc_psth_bit_mask_t psth = {0};
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &psth, ESP_AVRC_PT_CMD_PLAY);
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &psth, ESP_AVRC_PT_CMD_PAUSE);
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &psth, ESP_AVRC_PT_CMD_STOP);
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &psth, ESP_AVRC_PT_CMD_FORWARD);
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &psth, ESP_AVRC_PT_CMD_BACKWARD);
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &psth, ESP_AVRC_PT_CMD_VOL_UP);
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &psth, ESP_AVRC_PT_CMD_VOL_DOWN);
    esp_avrc_tg_set_psth_cmd_filter(ESP_AVRC_PSTH_FILTER_SUPPORTED_CMD, &psth);

    /* 注册音量变化通知能力 */
    esp_avrc_rn_evt_cap_mask_t evt_set = {0};
    esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &evt_set, ESP_AVRC_RN_VOLUME_CHANGE);
    esp_avrc_tg_set_rn_evt_cap(&evt_set);

    esp_a2d_source_init();                               /* A2DP 源 (本机作为播放源) */
    esp_a2d_register_callback(&bt_a2dp_a2d_cb);
    esp_a2d_source_register_data_callback(bt_a2dp_data_cb);   /* PCM 数据回调 */

    esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);   /* 初始不可被发现 */

    s_bt_ready = true;
    ESP_LOGI(BT_TAG, "蓝牙初始化完成"); 
}

/* ── 未连接时排水: 以 44.1kHz/16bit/双声道 真实速率丢弃 PCM ── */
#define DRAIN_BYTES_PER_SEC  (44100 * 2 * 2)

/* 5Hz 轮询: 全局音量有变化则同步到耳机 (音量待设窗口不推, 由连接时序统一在延迟后设置) */
static void bt_sync_volume(void)
{
    if (s_pending_vol) return;   /* 待设窗口内不推 */
    int32_t v = volume_get();
    if (v != s_last_sent_vol) {   /* 有变化才发送 */
        s_last_sent_vol = v;
        esp_avrc_ct_send_set_absolute_volume_cmd(APP_RC_CT_TL_GET_CAPS, (uint8_t)v);
    }
}

/* 未连接时按播放速率把音频任务产出的 PCM 丢弃, 防止流缓冲越积越多 */
static void bt_drain_pcm(void)
{
    static int64_t s_drain_last_us = 0;
    int64_t now = esp_timer_get_time();
    if (s_drain_last_us == 0) {
        s_drain_last_us = now;
        return;
    }
    int64_t dt = now - s_drain_last_us;
    s_drain_last_us = now;
    if (dt <= 0) return;

    /* 本次应丢弃的字节数 (按真实速率推算) */
    uint32_t budget = (uint32_t)(((uint64_t)DRAIN_BYTES_PER_SEC * (uint64_t)dt) / 1000000ULL);

    uint8_t tmp[256];
    while (budget > 0) {
        size_t want = budget > sizeof(tmp) ? sizeof(tmp) : budget;
        size_t got = xStreamBufferReceive(s_iface.pcm_stream, tmp, want, 0);
        if (got == 0) break;
        budget -= got;
    }
}

/* ── BT task entry (merged: commands + dispatch events) ── */
static void bt_a2dp_task(void *arg)
{
    /* 释放 BLE 控制器内存 (只用于 Classic BT) */
    esp_bt_controller_mem_release(ESP_BT_MODE_BLE);

    /* 初始化蓝牙控制器 */
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if (esp_bt_controller_init(&bt_cfg) != ESP_OK) {
        ESP_LOGE(BT_TAG, "蓝牙控制器初始化失败");
        vTaskDelete(NULL);
        return;
    }
    if (esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT) != ESP_OK) {
        ESP_LOGE(BT_TAG, "蓝牙控制器启用失败");
        vTaskDelete(NULL);
        return;
    }
    if (esp_bredr_tx_power_set(ESP_PWR_LVL_P9, ESP_PWR_LVL_P9) != ESP_OK) {
        ESP_LOGW(BT_TAG, "设置蓝牙发射功率失败");
    }

    /* 初始化并启用 Bluedroid 协议栈 */
    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_bluedroid_init_with_cfg(&bluedroid_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(BT_TAG, "Bluedroid 初始化失败: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }
    if (esp_bluedroid_enable() != ESP_OK) {
        ESP_LOGE(BT_TAG, "Bluedroid 启用失败");
        vTaskDelete(NULL);
        return;
    }

    /* 安全参数: 输入输出能力 (可显示/输入) + 可变 PIN */
    esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_IO;
    esp_bt_gap_set_security_param(param_type, &iocap, sizeof(uint8_t));

    esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_VARIABLE;
    esp_bt_pin_code_t pin_code;
    esp_bt_gap_set_pin(pin_type, 0, pin_code);

    char bda_str[18];
    ESP_LOGI(BT_TAG, "本机地址:[%s]", bda2str((uint8_t *)esp_bt_dev_get_address(), bda_str, sizeof(bda_str)));

    /* 接口句柄(队列/流/分发)已在 bt_a2dp_init 同步创建, 此处只做协议栈启动 */
    bt_a2dp_hdl_stack_up();

    bt_cmd_t          cmd;
    bt_dispatch_msg_t disp;
    QueueHandle_t     active;

    while (1) {
        if (s_connected) {
            /* 已连接: 200ms 超时轮询, 期间处理音量同步/流启停 */
            active = xQueueSelectFromSet(s_queue_set, pdMS_TO_TICKS(200));
            bt_sync_volume();
            if (active == NULL) {
                /* 连接时序: 等 AVRC 连上 → 再等 VOL_SET_DELAY_MS → 发音量 → 放行流启动.
                 * 兜底: AVRC 超时 VOL_WAIT_FALLBACK_MS 仍未连上则照常继续 */
                if (s_pending_vol) {
                    int64_t now = esp_timer_get_time();
                    bool ready = s_avrc_connected && s_avrc_at_us != 0 &&
                                 (now - s_avrc_at_us) >= VOL_SET_DELAY_MS * 1000LL;
                    bool fb = s_connect_at_us != 0 &&
                              (now - s_connect_at_us) >= VOL_WAIT_FALLBACK_MS * 1000LL;
                    if (ready || fb) {   /* 到点发音量, 放行流启动 */
                        int32_t v = volume_get();
                        s_last_sent_vol = v;
                        esp_avrc_ct_send_set_absolute_volume_cmd(APP_RC_CT_TL_GET_CAPS, (uint8_t)v);
                        ESP_LOGI(RC_TAG, "AVRC就绪后 %dms 发音量 %d, 放行流媒体启动",
                                 VOL_SET_DELAY_MS, v);
                        s_pending_vol = false;
                    }
                }
                if (!s_pending_vol) {   /* 音量窗口结束后, 按播放状态启停流 */
                    bool want = atomic_load_bool(&g_pcm_active);
                    if (want != s_stream_started) {
                        esp_a2d_media_ctrl(want ? ESP_A2D_MEDIA_CTRL_START
                                                : ESP_A2D_MEDIA_CTRL_SUSPEND);
                        s_stream_started = want;
                    }
                }
                continue;
            }
        } else {
            /* 未连接(含连接中): 仅排水(纯 CPU)。调制解调器睡眠由控制器自动管理, 不手动开关 */
            bool have_pcm = atomic_load_bool(&g_pcm_active) ||
                            xStreamBufferBytesAvailable(s_iface.pcm_stream) > 0;
            active = xQueueSelectFromSet(s_queue_set,
                                         have_pcm ? pdMS_TO_TICKS(100)
                                                  : pdMS_TO_TICKS(1000));
            if (active == NULL) {
                if (have_pcm) {
                    bt_drain_pcm();   /* 丢弃音频任务产出的 PCM */
                }
                continue;
            }
        }

        if (active == s_iface.cmd_queue) {   /* 处理命令 */
            xQueueReceive(s_iface.cmd_queue, &cmd, 0);

            switch (cmd.type) {
            case BT_CMD_SCAN: {   /* 开始扫描 (浏览模式) */
                if (s_connected) {
                    ESP_LOGW(BT_TAG, "已连接设备，忽略扫描命令");
                    send_state_rsp();
                    break;
                }
                if (s_scanning) {
                    ESP_LOGW(BT_TAG, "正在扫描中，忽略重复扫描命令");
                    break;
                }
                if (s_connect_target[0] != '\0') {
                    ESP_LOGW(BT_TAG, "正在进行连接扫描，忽略扫描命令");
                    break;
                }
                ESP_LOGI(BT_TAG, "开始设备搜索...");
                s_cache_count = 0;   /* 清空上次扫描缓存 */
                memset(s_cache_bda, 0, sizeof(s_cache_bda));
                memset(s_cache_name, 0, sizeof(s_cache_name));
                s_scanning = true;
                esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 3, 0);
                break;
            }
            case BT_CMD_STOP_SCAN: {   /* 停止扫描 */
                if (!s_scanning) {
                    ESP_LOGW(BT_TAG, "未在扫描中，忽略终止扫描命令");
                    send_state_rsp();
                    break;
                }
                ESP_LOGI(BT_TAG, "终止扫描...");
                if (s_connect_target[0] != '\0') {   /* 若在连接扫描则中止连接流程 */
                    memset(s_connect_target, 0, sizeof(s_connect_target));
                    s_pending_connect_scan = false;
                    s_connecting = false;
                    memset(s_connecting_name, 0, sizeof(s_connecting_name));
                    send_evt(BT_EVT_CONNECT_FAILED, NULL, -1);
                }
                esp_bt_gap_cancel_discovery();
                break;
            }
            case BT_CMD_CONNECT: {   /* 搜索并连接指定设备 */
                if (s_connected) {
                    ESP_LOGW(BT_TAG, "已连接设备，忽略连接命令");
                    break;
                }
                if (s_connect_target[0] != '\0') {
                    ESP_LOGW(BT_TAG, "正在连接中，忽略重复连接命令");
                    break;
                }
                ESP_LOGI(BT_TAG, "开始搜索并连接: %s", cmd.device_name);
                strncpy(s_connect_target, cmd.device_name,
                        sizeof(s_connect_target) - 1);   /* 记录目标名 */
                s_connect_target[sizeof(s_connect_target) - 1] = '\0';
                strncpy(s_connecting_name, cmd.device_name,
                        sizeof(s_connecting_name) - 1);
                s_connecting_name[sizeof(s_connecting_name) - 1] = '\0';
                s_connect_retry = 0;
                s_connect_found = false;
                s_connecting = true;

                if (s_scanning) {   /* 正在空闲扫描 → 取消后转连接扫描 */
                    s_pending_connect_scan = true;
                    esp_bt_gap_cancel_discovery();
                } else {
                    s_scanning = true;
                    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 1, 0);
                }
                break;
            }
            case BT_CMD_DISCONNECT: {   /* 断开连接 */
                if (!s_connected) {
                    ESP_LOGW(BT_TAG, "未连接设备，忽略断开命令");
                    break;
                }
                ESP_LOGI(BT_TAG, "正在断开连接...");
                esp_a2d_source_disconnect(s_peer_bda);
                break;
            }
            case BT_CMD_GET_STATE: {   /* 查询状态 */
                send_state_rsp();
                break;
            }
            default:
                break;
            }
        } else if (active == s_dispatch_queue) {   /* 处理协议栈事件 */
            xQueueReceive(s_dispatch_queue, &disp, 0);

            switch (disp.type) {
            case BT_DISPATCH_A2DP:
                bt_a2dp_hdl_a2d_evt(disp.event, disp.param);
                break;
            case BT_DISPATCH_AVRC:
                bt_a2dp_hdl_avrc_evt(disp.event, disp.param);
                break;
            case BT_DISPATCH_AVRC_TG:
                bt_a2dp_hdl_avrc_tg_evt(disp.event, disp.param);
                break;
            }

            if (disp.param) {
                free(disp.param);   /* 释放拷贝的参数 */
            }
        }
    }
}

/* 初始化蓝牙子系统: 同步创建接口句柄, 后台启动 BT 任务. 返回接口或 NULL. */
bt_a2dp_iface_t *bt_a2dp_init(void)
{
    /* 接口句柄同步创建: 让 app_main 立即返回, BT 控制器启动在后台与 LCD/UI 初始化重叠.
     * 失败由任务内 ESP_LOGE 记录, UI 照常启动 */
    s_iface.cmd_queue  = xQueueCreate(10, sizeof(bt_cmd_t));    /* 命令队列 */
    s_iface.evt_queue  = xQueueCreate(20, sizeof(bt_evt_t));    /* 事件队列 */
    s_iface.pcm_stream = xStreamBufferCreate(24 * 1024, 512);   /* PCM 流缓冲 (24KB, 触发 512B) */
    if (!s_iface.cmd_queue || !s_iface.evt_queue || !s_iface.pcm_stream) {
        return NULL;
    }

    s_dispatch_queue = xQueueCreate(10, sizeof(bt_dispatch_msg_t));   /* 内部事件分发 */
    s_queue_set      = xQueueCreateSet(8);                            /* 队列集 */
    if (!s_dispatch_queue || !s_queue_set) {
        return NULL;
    }
    xQueueAddToSet(s_iface.cmd_queue, s_queue_set);
    xQueueAddToSet(s_dispatch_queue, s_queue_set);

    if (xTaskCreatePinnedToCore(bt_a2dp_task, "bt_a2dp", 3072, NULL, 1, NULL, 0) != pdPASS) {
        return NULL;
    }

    return &s_iface;
}

/* 查询连接状态 */
bt_state_t bt_a2dp_get_state(void)
{
    if (!s_bt_ready) return BT_STATE_DISCONNECTED;   /* 协议栈未就绪视为未连接 */
    if (s_connected) return BT_STATE_CONNECTED;
    if (s_connecting) return BT_STATE_CONNECTING;
    return BT_STATE_DISCONNECTED;
}

bool bt_a2dp_is_connected(void)
{
    return s_connected && s_bt_ready;
}
