/* 临时文件: 链接期测量探针 (测完删除, 不属于正式代码)
 *
 * 为什么需要它:
 *   当前工程里没有任何代码引用 esp_wifi / esp_http_client, 链接器的 --gc-sections
 *   会把整个 WiFi + HTTP 栈丢掉. 因此"改完 sdkconfig 直接编译"这种构建根本量不出
 *   WiFi 到底吃多少 IRAM / DROM —— 它编出来的固件里压根没有 WiFi.
 *
 *   这里写一个运行期永远不执行 (由 volatile 开关挡住, 编译器无法常量折叠) 的函数,
 *   把网络升级最终会用到的符号全部引用一遍, 让链接器把真身拉进来.
 *   编完读 music_nano.map 里 .iram0.text / .flash.rodata 的实测值, 就是
 *   "加了网络升级之后"的真实占用.
 *
 * 配套临时改动 (三处, 测量完一起删):
 *   main/net_probe.c              ← 本文件
 *   main/CMakeLists.txt           ← SRCS 加 net_probe.c; PRIV_REQUIRES 加网络组件
 *   main/main.c                   ← app_main 最前面调一次 net_probe_maybe()
 */

#include <stdbool.h>
#include <stddef.h>

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"

static volatile bool s_net_probe_enabled;   /* 恒为 false, 但编译器不知道, 所以不能优化掉调用 */

void net_probe_maybe(void)
{
    if (!s_net_probe_enabled) {
        return;
    }

    /* ---- WiFi 驱动: 初始化 + STA 连接 ---- */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_start();
    esp_wifi_connect();
    esp_wifi_stop();
    esp_wifi_deinit();

    /* ---- 网络接口 + 事件循环 ---- */
    esp_netif_init();
    esp_netif_create_default_wifi_sta();
    esp_event_loop_create_default();

    /* ---- HTTP 流式下载 (明文 http, 不引 TLS) ---- */
    esp_http_client_config_t hc = {
        .url        = "http://192.168.0.1/music_nano.bin",
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&hc);
    if (c) {
        esp_http_client_open(c, 0);
        esp_http_client_fetch_headers(c);
        esp_http_client_read(c, NULL, 0);
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
    }

    /* ---- OTA: 写入非运行槽 ---- */
    const esp_partition_t *p = esp_ota_get_next_update_partition(NULL);
    esp_ota_handle_t h = 0;
    esp_ota_begin(p, OTA_WITH_SEQUENTIAL_WRITES, &h);
    esp_ota_write(h, NULL, 0);
    esp_ota_end(h);
    esp_ota_set_boot_partition(p);
}
