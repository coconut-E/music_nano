/* WiFi 密码记忆 (NVS) —— 设计说明见 wifi_cred.h */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "esp_log.h"
#include "nvs.h"

#include "wifi_cred.h"

static const char *TAG = "WIFI_CRED";

/* 命名空间名同样受 NVS 键名规则约束: ASCII, 最长 15 字符 */
#define WIFI_CRED_NS  "wifi_cred"

/* 一条记录. 定长 100B, 一次 nvs_set_blob 写完整, 不会出现"改了 SSID 没改密码".
 * ssid/pass 后面留一个字节的余量, 读取时补 '\0', 便于直接当 C 字符串用. */
typedef struct {
    uint8_t ssid_len;
    uint8_t pass_len;
    char    ssid[33];      /* 802.11 SSID 最长 32 字节 */
    char    pass[65];      /* WPA2 密码最长 63 字符 */
} wifi_cred_t;

static nvs_handle_t s_nvs;
static bool         s_ready;      /* 句柄可用 */
static bool         s_warned;     /* nvs_open 失败只报一次, 别每次选网都刷屏 */

static bool ensure_open(void)
{
    if (s_ready) return true;
    if (nvs_open(WIFI_CRED_NS, NVS_READWRITE, &s_nvs) == ESP_OK) {
        s_ready = true;
        return true;
    }
    if (!s_warned) {
        s_warned = true;
        ESP_LOGW(TAG, "nvs_open(%s) 失败, 密码记忆功能禁用 (连接本身不受影响)", WIFI_CRED_NS);
    }
    return false;
}

/* FNV-1a 32 位: 把任意字节的 SSID 压成 8 位 hex 的键名.
 * NVS 键名只能是 ASCII 且最长 15 字符, 中文 SSID 没法直接当键, 所以走哈希;
 * 完整 SSID 存在值里, 读出来再比对, 碰撞只会导致"没命中". */
static uint32_t fnv1a(const char *s)
{
    uint32_t h = 2166136261u;          /* FNV offset basis */
    while (*s) {
        h ^= (uint8_t)(*s++);
        h *= 16777619u;                /* FNV prime */
    }
    return h;
}

/* 键名 9 个字符: 'c' + 8 位十六进制 */
static void make_key(const char *ssid, char *key, size_t key_sz)
{
    snprintf(key, key_sz, "c%08x", (unsigned)fnv1a(ssid));
}

bool wifi_cred_load(const char *ssid, char *pass, size_t pass_sz)
{
    if (!ssid || !pass || pass_sz == 0) return false;
    if (!ensure_open()) return false;

    char key[16];
    make_key(ssid, key, sizeof key);

    wifi_cred_t rec;
    size_t      len = sizeof rec;
    if (nvs_get_blob(s_nvs, key, &rec, &len) != ESP_OK) return false;
    if (len != sizeof rec) return false;          /* 结构变过版本, 当没命中 */

    size_t sl = strlen(ssid);
    if (sl > sizeof rec.ssid) return false;
    if (rec.ssid_len != sl) return false;
    if (memcmp(rec.ssid, ssid, sl) != 0) return false;   /* 哈希碰撞: 当没命中 */

    if (rec.pass_len >= pass_sz) return false;
    memcpy(pass, rec.pass, rec.pass_len);
    pass[rec.pass_len] = '\0';

    ESP_LOGI(TAG, "命中 \"%s\" (密码 %u 字符, 键 %s)", ssid, (unsigned)rec.pass_len, key);
    return true;
}

bool wifi_cred_save(const char *ssid, const char *pass)
{
    if (!ssid || !pass) return false;

    size_t sl = strlen(ssid);
    size_t pl = strlen(pass);
    if (sl == 0 || sl > 32 || pl > 64) {
        ESP_LOGW(TAG, "长度非法 (SSID %u, 密码 %u), 不记", (unsigned)sl, (unsigned)pl);
        return false;
    }
    if (!ensure_open()) return false;

    char key[16];
    make_key(ssid, key, sizeof key);

    wifi_cred_t rec;
    memset(&rec, 0, sizeof rec);
    rec.ssid_len = (uint8_t)sl;
    rec.pass_len = (uint8_t)pl;
    memcpy(rec.ssid, ssid, sl);
    memcpy(rec.pass, pass, pl);

    esp_err_t err = nvs_set_blob(s_nvs, key, &rec, sizeof rec);
    if (err == ESP_OK) err = nvs_commit(s_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "记住 \"%s\" 失败: %s", ssid, esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "记住 \"%s\" (密码 %u 字符, 键 %s)", ssid, (unsigned)pl, key);
    return true;
}

bool wifi_cred_remove(const char *ssid)
{
    if (!ssid) return false;
    if (!ensure_open()) return false;

    char key[16];
    make_key(ssid, key, sizeof key);

    esp_err_t err = nvs_erase_key(s_nvs, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "没有 \"%s\" 的记录, 无需删除", ssid);
        return true;
    }
    if (err == ESP_OK) err = nvs_commit(s_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "删除 \"%s\" 失败: %s", ssid, esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "已删除 \"%s\" (键 %s)", ssid, key);
    return true;
}
