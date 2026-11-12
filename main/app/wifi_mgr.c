/* WiFi 会话管理 (只服务设置页的「检查更新 → 连接 WiFi」流程)
 *
 * 设计要点见 wifi_mgr.h 的注释; 这里只补充实现上的三个坑:
 *
 * 1) 一次会话一个任务. ACQUIRE 在任务里做, RELEASE 做完任务自己 vTaskDelete.
 *    这样"资源是否释放"只取决于任务是否退出, 不需要额外的引用计数, 也不会出现
 *    "界面已经删了但 WiFi 还在跑"的中间态.
 *
 * 2) 扫描/连接都是阻塞调用, 天然不可中断 —— 这正是需求里"任何操作 WiFi 的行为
 *    都不允许中途退出"想要的语义: 命令队列串行, 没有取消命令, 只有跑完.
 *    连接还有 20 秒上限, 避免密码错误时无限等待.
 *
 * 3) 事件只走队列, 不回调 UI. LVGL 不是线程安全的, 由 UI 侧的定时器轮询取事件,
 *    在 LVGL 任务里再碰界面. */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "update.h"
#include "wifi_mgr.h"

static const char *TAG = "WIFI";

/* 6144: 这个任务现在还跑 TLS 握手 (mbedtls) + esp_http_client 的调用链,
 * 5120 在握手里余量不够. 任务只在「检查更新」流程里存在, 用完就删. */
#define WIFI_TASK_STACK     6144
#define WIFI_TASK_PRIO      2
#define WIFI_TASK_CORE      0          /* 和 LVGL (core 1) 分开, 扫描时界面照常动 */
#define WIFI_CONNECT_MS     20000      /* 连接 + 等 IP 的上限 */
#define WIFI_SCAN_MAX_RAW   32         /* 驱动一次最多返回多少条 (再自己筛/去重) */

#define BIT_GOT_IP  BIT0
#define BIT_FAIL    BIT1

typedef enum {
    CMD_SCAN = 0,
    CMD_CONNECT,
    CMD_UPDATE_CHECK,   /* GET 版本接口, 只查不下载 */
    CMD_UPDATE_START,   /* 下载 + 写备份槽 + 切槽 (成功后由 UI 重启) */
    CMD_RELEASE,
} wifi_cmd_type_t;

typedef struct {
    wifi_cmd_type_t type;
    char            ssid[33];
    char            pass[65];
} wifi_cmd_t;

typedef struct {
    wifi_ev_t ev;
    int       arg;
} wifi_evt_t;

/* 扫描结果 (按 RSSI 从强到弱, 已按 SSID 去重) */
typedef struct {
    char    ssid[33];
    int8_t  rssi;
    bool    open;
} wifi_ap_t;

static QueueHandle_t        s_cmd_q  = NULL;
static QueueHandle_t        s_evt_q  = NULL;
static EventGroupHandle_t   s_eg     = NULL;
static TaskHandle_t         s_task   = NULL;
static volatile bool        s_alive  = false;

static esp_netif_t         *s_netif        = NULL;
static bool                 s_netif_inited = false;   /* esp_netif_init 是我们做的 */
static bool                 s_loop_owned   = false;   /* 默认事件循环是我们建的 */
static bool                 s_wifi_inited  = false;
static volatile bool        s_connecting   = false;   /* 只在连接等待期间置位 */
static volatile int         s_disc_reason  = 0;
static esp_event_handler_instance_t s_h_wifi = NULL;
static esp_event_handler_instance_t s_h_ip   = NULL;

static wifi_ap_t s_ap[WIFI_MGR_MAX_APS];
static int       s_ap_n = 0;

/* ── 小工具 ── */

static void post_evt(wifi_ev_t ev, int arg)
{
    if (!s_evt_q) return;
    wifi_evt_t e = { .ev = ev, .arg = arg };
    /* 满了就丢: UI 只在流程里读, 正常不会溢出; 真溢出说明界面已经不在等了 */
    xQueueSend(s_evt_q, &e, 0);
}

static void log_heap(const char *stage, size_t before)
{
    size_t now  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t lrg  = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t dma  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    size_t dlrg = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    ESP_LOGI(TAG, "%s: 内部堆 %u → %u B (差 %d, 最大块 %u), DMA 可用 %u B (最大连续块 %u)",
             stage, (unsigned)before, (unsigned)now, (int)now - (int)before,
             (unsigned)lrg, (unsigned)dma, (unsigned)dlrg);
}

/* ── 事件 ── */

static void wifi_event_cb(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    /* 关联成功: 显式叫一次 DHCP 客户端.
     * 本来这活是 esp_netif_create_default_wifi_sta() 注册的默认回调干 (WIFI_EVENT_STA_CONNECTED
     * → esp_netif_action_connected → dhcpc_start), 但那是"进程级只注册一次"的
     * (wifi_default.c: set_default_wifi_handlers 有 wifi_default_handlers_set 早退),
     * 而我们每次退出都会把默认事件循环一起删掉 —— 于是第二、三次进流程时这些默认回调
     * 可能已经不在了, 表现为: 关联成功、手机显示已连接, 但 20 秒都拿不到 IP. */
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        if (s_netif) {
            esp_err_t e = esp_netif_dhcpc_start(s_netif);
            ESP_LOGI(TAG, "已关联, 启动 DHCP: %s", esp_err_to_name(e));
        }
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        s_disc_reason = d->reason;
        if (s_connecting) {
            xEventGroupSetBits(s_eg, BIT_FAIL);
        } else {
            /* 连上之后又被踢掉 (AP 重启/信号丢失): 只记日志, UI 还没做掉线处理 */
            ESP_LOGW(TAG, "WiFi 断开 (reason=%d), 非连接阶段", d->reason);
        }
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "拿到 IP: " IPSTR, IP2STR(&e->ip_info.ip));
        if (s_connecting) xEventGroupSetBits(s_eg, BIT_GOT_IP);
    }
}

/* ── ACQUIRE ── */

/* 初始化前至少要留多少内部 RAM.
 * 实测: 用 IDF 默认缓冲数 (静态 RX 10 / 动态 RX-TX 32 / 管理缓冲 32) 时,
 * esp_wifi_init 在分配静态 RX 缓冲那一步就 ESP_ERR_NO_MEM, 而且它自己 deinit 失败,
 * 白扔掉 ~19KB (之后只剩 28KB / 最大块 11KB, 只能重启恢复).
 * 缓冲数调小后 WiFi 稳态只占 ~25KB, 所以留 30KB 门槛: 不够就干脆不初始化,
 * 免得半路失败留下收不回的残块. */
#define WIFI_MIN_INTERNAL_RAM  30000

static bool wifi_acquire(void)
{
    size_t heap0  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t dma    = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    size_t dma_lg = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);

    ESP_LOGI(TAG, "准备初始化 WiFi: 内部堆 %u B, DMA 可用 %u B (最大块 %u B)",
             (unsigned)heap0, (unsigned)dma, (unsigned)dma_lg);

    if (heap0 < WIFI_MIN_INTERNAL_RAM) {
        ESP_LOGE(TAG, "内部 RAM 不足 (%u < %u B), 放弃初始化",
                 (unsigned)heap0, (unsigned)WIFI_MIN_INTERNAL_RAM);
        return false;
    }

    esp_err_t err = esp_netif_init();
    if (err == ESP_OK) {
        s_netif_inited = true;
    } else if (err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init 失败: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_event_loop_create_default();
    if (err == ESP_OK) {
        s_loop_owned = true;
    } else if (err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "创建默认事件循环失败: %s", esp_err_to_name(err));
        return false;
    }

    s_netif = esp_netif_create_default_wifi_sta();
    if (!s_netif) {
        ESP_LOGE(TAG, "创建默认 STA netif 失败");
        return false;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    /* 运行期把管理帧 RX 缓冲从 2 降到 1 (每块 ~1.6KB).
     * 实测模型: esp_wifi_init 的固定开销 ≈19KB(DMA 内部堆) + 1.6KB × (static_rx + rx_mgmt);
     * 本机进流程时 DMA 可用只有 ~37KB, 按 Kconfig 的 4+2 要 28.6KB, 余量本来够,
     * 但界面先行占掉 ~10KB 就会在 "Init static rx buffer" 那步失败 —— 所以 UI 侧也
     * 改成了先建极简占位页. 这里再压 1.6KB 当保险, 让 4+1 = 27KB.
     * (Kconfig 的 range 是 1..10, 1 合法) */
    cfg.rx_mgmt_buf_num = 1;

    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        /* NO_MEM 时 IDF 内部 deinit 会失败, 已经分配的那部分收不回来 —— 只能重启恢复 */
        ESP_LOGE(TAG, "esp_wifi_init 失败: %s", esp_err_to_name(err));
        if (err == ESP_ERR_NO_MEM) {
            log_heap("初始化失败后 (残块收不回)", heap0);
        }
        return false;
    }
    s_wifi_inited = true;

    /* 驱动自己的配置只放 RAM: 不让 esp_wifi 往它自己的 NVS 里写 SSID/密码,
     * 每次会话都从干净配置开始 (否则上一次连过的网络会被驱动自动重连).
     * 用户"记住密码"是另一回事, 存在独立的 wifi_cred 命名空间里, 见 app/wifi_cred.c */
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_mode 失败: %s", esp_err_to_name(err));
        return false;
    }

    wifi_config_t wc;
    memset(&wc, 0, sizeof wc);          /* 清掉 NVS 里可能残留的旧配置 */
    esp_wifi_set_config(WIFI_IF_STA, &wc);

    esp_wifi_set_ps(WIFI_PS_NONE);      /* 扫描/连接期间不要省电休眠, 响应快 */

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        wifi_event_cb, NULL, &s_h_wifi);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        wifi_event_cb, NULL, &s_h_ip);

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start 失败: %s", esp_err_to_name(err));
        return false;
    }

    log_heap("WiFi 已启动", heap0);
    return true;
}

/* ── RELEASE ── */

static void wifi_release(void)
{
    size_t heap0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    /* 先摘事件: 免得下面的 disconnect 又触发一次 STA_DISCONNECTED */
    if (s_h_wifi) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_h_wifi);
        s_h_wifi = NULL;
    }
    if (s_h_ip) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_h_ip);
        s_h_ip = NULL;
    }

    /* 先把 DHCP 租约还回去 (lwIP 会发一个 DHCP RELEASE), 再断开.
     * 不还的话, 手机的 DHCP 会一直记着"这个 MAC 用的是上次分的那个 IP"; 下次重连时它把
     * OFFER 直接单播到那个旧 IP, 而我们此刻 IP 还是 0.0.0.0 —— 报文在 IP 层就被丢掉,
     * DHCP 客户端永远收不到. 表现就是: 关联成功、DHCP 状态 STARTED、永远 0.0.0.0,
     * 也就是"有时能连有时连不上"的真正原因. */
    if (s_netif) {
        esp_err_t e = esp_netif_dhcpc_stop(s_netif);
        ESP_LOGI(TAG, "断开前释放 DHCP 租约: %s", esp_err_to_name(e));
    }

    if (s_wifi_inited) {
        esp_wifi_disconnect();          /* 未连接时返回错误码, 忽略 */
        esp_wifi_stop();
        esp_wifi_deinit();
        s_wifi_inited = false;
    }

    if (s_netif) {
        esp_netif_destroy_default_wifi(s_netif);
        s_netif = NULL;
    }

    if (s_loop_owned) {
        esp_event_loop_delete_default();
        s_loop_owned = false;
    }

    if (s_netif_inited) {
        esp_netif_deinit();
        s_netif_inited = false;
    }

    s_connecting = false;
    log_heap("WiFi 已释放", heap0);
}

/* ── SCAN ── */

static void wifi_do_scan(void)
{
    wifi_scan_config_t sc;
    memset(&sc, 0, sizeof sc);
    sc.show_hidden = false;

    ESP_LOGI(TAG, "开始扫描…");
    esp_err_t err = esp_wifi_scan_start(&sc, true);   /* 阻塞到扫描结束 */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "扫描失败: %s", esp_err_to_name(err));
        post_evt(WIFI_EV_SCAN_FAIL, (int)err);
        return;
    }

    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (num > WIFI_SCAN_MAX_RAW) num = WIFI_SCAN_MAX_RAW;

    wifi_ap_record_t *recs = NULL;
    uint16_t got = 0;
    if (num > 0) {
        recs = heap_caps_malloc((size_t)num * sizeof(wifi_ap_record_t), MALLOC_CAP_8BIT);
    }
    if (recs) {
        got = num;
        /* 这个调用同时释放驱动内部的扫描结果缓冲 */
        if (esp_wifi_scan_get_ap_records(&got, recs) != ESP_OK) got = 0;
    } else {
        ESP_LOGW(TAG, "扫描结果缓冲分配失败");
    }

    /* 去重 (同一 SSID 多 BSSID 只留最强的) + 按 RSSI 插入排序 */
    s_ap_n = 0;
    for (uint16_t i = 0; i < got; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        if (ssid[0] == '\0') continue;              /* 隐藏 SSID 不列 */

        int dup = -1;
        for (int k = 0; k < s_ap_n; k++) {
            if (strncmp(s_ap[k].ssid, ssid, 32) == 0) { dup = k; break; }
        }
        if (dup >= 0) {
            if (recs[i].rssi > s_ap[dup].rssi) s_ap[dup].rssi = recs[i].rssi;
            continue;
        }

        int pos = s_ap_n;
        if (pos >= WIFI_MGR_MAX_APS) continue;
        while (pos > 0 && s_ap[pos - 1].rssi < recs[i].rssi) {
            s_ap[pos] = s_ap[pos - 1];
            pos--;
        }
        memset(&s_ap[pos], 0, sizeof s_ap[pos]);
        strncpy(s_ap[pos].ssid, ssid, sizeof s_ap[pos].ssid - 1);
        s_ap[pos].rssi = recs[i].rssi;
        s_ap[pos].open = (recs[i].authmode == WIFI_AUTH_OPEN);
        s_ap_n++;
    }

    if (recs) heap_caps_free(recs);

    ESP_LOGI(TAG, "扫描完成: 原始 %d 条, 去重后 %d 个网络", (int)got, s_ap_n);
    for (int i = 0; i < s_ap_n; i++) {
        ESP_LOGI(TAG, "  [%d] %-24s %d dBm%s", i, s_ap[i].ssid, s_ap[i].rssi,
                 s_ap[i].open ? " (开放)" : "");
    }

    post_evt(WIFI_EV_SCAN_DONE, s_ap_n);
}

/* ── CONNECT ── */

static void wifi_do_connect(const char *ssid, const char *pass)
{
    wifi_config_t wc;
    memset(&wc, 0, sizeof wc);
    /* 目标缓冲是定长数组且已清零: 按 strnlen 截断后 memcpy, 保证不以 0 结尾
     * (strncpy 在 -Werror=stringop-truncation 下会报"可能没截断") */
    size_t sl = strnlen(ssid, sizeof wc.sta.ssid - 1);
    memcpy(wc.sta.ssid, ssid, sl);
    size_t pl = strnlen(pass, sizeof wc.sta.password - 1);
    memcpy(wc.sta.password, pass, pl);
    /* 只要求开放/WPA2 及以上; 具体能不能连上由 AP 与密码决定 */
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "下发配置失败: %s", esp_err_to_name(err));
        post_evt(WIFI_EV_CONNECT_FAIL, -2);
        return;
    }

    s_disc_reason = 0;
    xEventGroupClearBits(s_eg, BIT_GOT_IP | BIT_FAIL);
    s_connecting = true;

    ESP_LOGI(TAG, "开始连接 \"%s\": DMA 可用 %u B (最大连续块 %u)",
             ssid,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        s_connecting = false;
        ESP_LOGE(TAG, "esp_wifi_connect 失败: %s", esp_err_to_name(err));
        post_evt(WIFI_EV_CONNECT_FAIL, -3);
        return;
    }

    /* 等 IP: 上限 20 秒. IP 由 WIFI_EVENT_STA_CONNECTED 里显式启动的 DHCP 客户端拿,
     * 拿到置 BIT_GOT_IP, 被断开置 BIT_FAIL (密码错就是这时知道的). */
    EventBits_t bits = xEventGroupWaitBits(s_eg, BIT_GOT_IP | BIT_FAIL, pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(WIFI_CONNECT_MS));
    s_connecting = false;

    if (bits & BIT_GOT_IP) {
        ESP_LOGI(TAG, "连接 \"%s\" 成功", ssid);
        post_evt(WIFI_EV_CONNECTED, 0);
        return;
    }

    /* 失败: 主动断开, 别把 4 次握手重传留在后台 */
    esp_wifi_disconnect();

    int reason = (bits & BIT_FAIL) ? s_disc_reason : -1;   /* -1 = 本地超时 */
    ESP_LOGW(TAG, "连接 \"%s\" 失败 (reason=%d%s)", ssid, reason,
             reason == 15 ? " 密码错误" : (reason == 201 ? " 找不到 AP" : ""));
    post_evt(WIFI_EV_CONNECT_FAIL, reason);
}

/* ── 任务 ── */

static void wifi_task(void *arg)
{
    (void)arg;

    if (wifi_acquire()) {
        post_evt(WIFI_EV_READY, 0);
    } else {
        post_evt(WIFI_EV_ERROR, 0);
        /* 初始化失败也继续等 RELEASE: 让 UI 的"退出"流程保持同一条路径 */
    }

    for (;;) {
        wifi_cmd_t cmd;
        if (xQueueReceive(s_cmd_q, &cmd, pdMS_TO_TICKS(1000)) != pdTRUE) {
            continue;
        }

        if (cmd.type == CMD_RELEASE) break;

        if (!s_wifi_inited) {               /* 初始化失败后不该再有新命令 */
            int code = (cmd.type == CMD_SCAN)  ? WIFI_EV_SCAN_FAIL
                     : (cmd.type == CMD_CONNECT) ? WIFI_EV_CONNECT_FAIL
                     : WIFI_EV_UPDATE_FAIL;
            post_evt((wifi_ev_t)code, -4);
            continue;
        }

        if (cmd.type == CMD_SCAN) {
            wifi_do_scan();
        } else if (cmd.type == CMD_CONNECT) {
            wifi_do_connect(cmd.ssid, cmd.pass);
        } else if (cmd.type == CMD_UPDATE_CHECK) {
            /* 版本查询: 阻塞到拿到结果, 结果码原样回给 UI (0 = 成功, 见 update.h) */
            post_evt(WIFI_EV_UPDATE_CHECKED, update_check());
        } else {
            int r = update_start();
            post_evt(r == UPD_OK ? WIFI_EV_UPDATE_DONE : WIFI_EV_UPDATE_FAIL, r);
        }
    }

    wifi_release();
    post_evt(WIFI_EV_RELEASED, 0);

    s_task  = NULL;
    s_alive = false;
    vTaskDelete(NULL);
}

/* ── 对外接口 ── */

void wifi_mgr_start_session(void)
{
    if (!s_cmd_q) s_cmd_q = xQueueCreate(4, sizeof(wifi_cmd_t));
    if (!s_evt_q) s_evt_q = xQueueCreate(8, sizeof(wifi_evt_t));
    if (!s_eg)    s_eg    = xEventGroupCreate();
    if (!s_cmd_q || !s_evt_q || !s_eg) {
        ESP_LOGE(TAG, "队列/事件组创建失败");
        return;
    }

    if (s_alive) return;    /* 已在会话里 */

    s_alive = true;
    if (xTaskCreatePinnedToCore(wifi_task, "wifi", WIFI_TASK_STACK, NULL,
                                WIFI_TASK_PRIO, &s_task, WIFI_TASK_CORE) != pdPASS) {
        s_alive = false;
        ESP_LOGE(TAG, "WiFi 任务创建失败");
        post_evt(WIFI_EV_ERROR, 0);
    }
}

void wifi_mgr_scan(void)
{
    if (!s_alive || !s_cmd_q) return;
    wifi_cmd_t cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.type = CMD_SCAN;
    if (xQueueSend(s_cmd_q, &cmd, 0) != pdTRUE) ESP_LOGW(TAG, "扫描命令投递失败 (队列满)");
}

void wifi_mgr_connect(const char *ssid, const char *password)
{
    if (!s_alive || !s_cmd_q) return;
    wifi_cmd_t cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.type = CMD_CONNECT;
    /* 同 wifi_do_connect: 定长数组 + 已清零, 按 strnlen 截断后拷贝 */
    size_t sl = ssid ? strnlen(ssid, sizeof cmd.ssid - 1) : 0;
    if (sl) memcpy(cmd.ssid, ssid, sl);
    size_t pl = password ? strnlen(password, sizeof cmd.pass - 1) : 0;
    if (pl) memcpy(cmd.pass, password, pl);
    if (xQueueSend(s_cmd_q, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "连接命令投递失败 (队列满)");
        post_evt(WIFI_EV_CONNECT_FAIL, -5);
    }
}

/* 版本查询 / 下载升级: 都只是投命令, 结果走事件 (WIFI_EV_UPDATE_*) */
void wifi_mgr_update_check(void)
{
    if (!s_alive || !s_cmd_q) return;
    wifi_cmd_t cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.type = CMD_UPDATE_CHECK;
    if (xQueueSend(s_cmd_q, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "版本查询命令投递失败 (队列满)");
        post_evt(WIFI_EV_UPDATE_FAIL, -5);
    }
}

void wifi_mgr_update_start(void)
{
    if (!s_alive || !s_cmd_q) return;
    wifi_cmd_t cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.type = CMD_UPDATE_START;
    if (xQueueSend(s_cmd_q, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "升级命令投递失败 (队列满)");
        post_evt(WIFI_EV_UPDATE_FAIL, -5);
    }
}

void wifi_mgr_release(void)
{
    if (!s_alive) {
        post_evt(WIFI_EV_RELEASED, 0);   /* 没有会话: 直接告诉 UI "已释放" */
        return;
    }
    if (!s_cmd_q) return;
    wifi_cmd_t cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.type = CMD_RELEASE;
    if (xQueueSend(s_cmd_q, &cmd, 0) != pdTRUE) ESP_LOGW(TAG, "释放命令投递失败 (队列满)");
}

bool wifi_mgr_session_active(void)
{
    return s_alive;
}

bool wifi_mgr_poll(wifi_ev_t *ev, int *arg)
{
    if (!s_evt_q) return false;
    wifi_evt_t e;
    if (xQueueReceive(s_evt_q, &e, 0) != pdTRUE) return false;
    if (ev)  *ev  = e.ev;
    if (arg) *arg = e.arg;
    return true;
}

void wifi_mgr_flush_events(void)
{
    wifi_evt_t e;
    if (!s_evt_q) return;
    while (xQueueReceive(s_evt_q, &e, 0) == pdTRUE) { /* 丢掉 */ }
}

int wifi_mgr_ap_count(void)
{
    return s_ap_n;
}

const char *wifi_mgr_ap_ssid(int idx)
{
    if (idx < 0 || idx >= s_ap_n) return "";
    return s_ap[idx].ssid;
}

bool wifi_mgr_ap_open(int idx)
{
    if (idx < 0 || idx >= s_ap_n) return false;
    return s_ap[idx].open;
}
