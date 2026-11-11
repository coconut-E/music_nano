#ifndef __WIFI_CRED_H__
#define __WIFI_CRED_H__

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* WiFi 密码记忆 (NVS).
 *
 * 需求: 连接成功就记住 "SSID + 密码"; 下次选同一个 SSID 时直接用记住的密码连接,
 * 不再让用户重新输; 如果拿记住的密码连不上, 就把这条记录删掉.
 *
 * 为什么键名不是 SSID
 * -------------------
 * NVS 的键名只能是 ASCII, 最长 15 字符, 而 WiFi 名可以是中文/任意 32 字节.
 * 所以键名 = "c" + SSID 的 FNV-1a 32 位哈希 (9 个字符), 完整 SSID 放在值里面;
 * 读出来先比对 SSID, 哈希撞了就当没命中 (可能"记不住", 但绝不会连错网).
 *
 * 值格式: 定长结构, 一次 nvs_set_blob 写完, 天然原子.
 *
 * 密码是明文存在 NVS 里 (本机没开 NVS 加密) —— 要"记住密码"只能这样.
 * 注意和驱动那边不是一回事: esp_wifi 的配置仍然只放 RAM (WIFI_STORAGE_RAM),
 * 不写它自己的 NVS. */

/* 命中就返回 true 并把密码拷进 pass (至少 65 字节). 没有记录/SSID 不符 → false. */
bool wifi_cred_load(const char *ssid, char *pass, size_t pass_sz);

/* 连接成功后记住. 写失败只记日志, 不影响连接本身. */
bool wifi_cred_save(const char *ssid, const char *pass);

/* 记住的密码连不上时删掉这条. 本来就没有记录也算成功. */
bool wifi_cred_remove(const char *ssid);

#ifdef __cplusplus
}
#endif

#endif
