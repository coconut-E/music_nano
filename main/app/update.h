#ifndef __UPDATE_H__
#define __UPDATE_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 网络 OTA: 查自建服务器的版本清单 + 分段下载 + 写入备份槽
 *
 * 调用约束
 * --------
 * update_check() / update_start() 只能在 wifi_mgr 的 worker 任务里跑:
 *   - 一次 TLS 握手 + 2.2MB 下载要几十秒, 放在 LVGL 任务里界面就死了;
 *   - 写 flash 时 cache 会关, 必须能主动让出 CPU (见 update.c 的"分步写").
 * UI 侧只读 update_status() (100ms 轮询), 绝不跨任务碰 LVGL.
 *
 * 两个 API (服务器和固件是配套的, 见 docs/OTA_TEST_zh.md)
 * -----------------------------------------------------
 *   1) 清单:  GET https://coconute.dpdns.org/ota/v1/manifest.json?channel=stable&device=<MAC>
 *             → {"schema":1,"version":…,"size":…,"sha256":…,"url":…, …}
 *   2) 固件:  GET <清单里的 url>, 带 Range, 分 OTA_BLOCKS(5) 段拉
 * 地址编译期写死 (update.c 的 UPD_API), 不存 NVS, 也没有串口命令能改 ——
 * 换服务器 = 改那一行 + 重编一版.
 *
 * 一次流程
 * --------
 *   update_check(): GET 清单 → 比本机版本: 不新 → phase=UPD_LATEST;
 *      更新 → phase=UPD_READY 等用户点按钮
 *   update_start(): 5 段 Range 下载, 每段 64KB 一块进 PSRAM → 停读 → 16KB 一步
 *      写进备份槽 → esp_ota_end 校验 → sha256 对账 → esp_ota_set_boot_partition
 *
 * 失败时 otadata 与两个槽都不会被"切走": update_start() 只在最后一步写 otadata,
 * 中途失败只留下备份槽里的一段垃圾, 下次启动照旧跑老槽. */

typedef enum {
    UPD_OK = 0,
    UPD_E_NET       = -1,   /* 连不上 / TLS 握手失败 / 读超时 */
    UPD_E_HTTP      = -2,   /* 状态码不对 (清单要 200, 下载要 206; 403/404 也在这里) */
    UPD_E_PARSE     = -3,   /* 清单过大 / 缺字段 / schema 不是 1 */
    UPD_E_NO_REL    = -4,   /* 该频道没有发布 (服务器 404) */
    UPD_E_NO_ASSET  = -5,   /* 清单里没有可用的下载地址 (缺 url, 或固件 404) */
    UPD_E_NOMEM     = -6,   /* PSRAM 暂存块 / 内部 RAM 弹跳块申请失败 */
    UPD_E_SHORT     = -7,   /* 某一段没收全: 长度不符 / 提前 EOF / 总长对不上 */
    UPD_E_WRITE     = -8,   /* esp_ota_begin / esp_ota_write 失败 */
    UPD_E_VERIFY    = -9,   /* 镜像校验失败 (esp_ota_end 或 sha256 对不上) */
    UPD_E_SWITCH    = -10,  /* 找备份槽 / 写 otadata 失败 */
    UPD_E_VERIFYING = -11,  /* 当前跑的是"还没确认的新固件", 先别升 */
} upd_err_t;

typedef enum {
    UPD_IDLE = 0,
    UPD_CHECKING,       /* 正在 GET 版本接口 */
    UPD_LATEST,         /* 已是最新 (new_ver 也填了, 界面上可以显示) */
    UPD_READY,          /* 查到新版本, 等用户点「更新」 */
    UPD_DOWNLOADING,    /* 正在收数据 (收满一块会转 UPD_WRITING) */
    UPD_WRITING,        /* 正在写 flash */
    UPD_DONE,           /* 已写进备份槽并切好启动项, 等重启 */
    UPD_FAIL,
} upd_phase_t;

/* 进度: worker 单写, UI 单读. 只有 phase 是"闸门"——
 * 写完后才置 phase, 读侧看到 phase 变化时其余字段已经填好. */
typedef struct {
    volatile upd_phase_t phase;
    volatile uint32_t    total;    /* 固件总字节 (来自 JSON 的 size) */
    volatile uint32_t    dl;       /* 已下载字节 */
    volatile uint32_t    wr;       /* 已写入 flash 的字节 */
    volatile int         err;      /* upd_err_t */
    volatile int         http;     /* 最近一次 HTTP 状态码 (0 = 还没发请求) */
    char cur_ver[33];              /* 本机版本 (esp_app_get_description, 最长 32) */
    char new_ver[33];              /* 服务器上最新固件的版本 */
} upd_status_t;

const upd_status_t *update_status(void);

/* 进流程时清一遍 (phase/err/计数/版本串) */
void update_reset(void);

/* 查版本. 返回 upd_err_t; 成功时看 phase (UPD_LATEST / UPD_READY) */
int update_check(void);

/* 下载 + 写入 + 切槽. 只有 phase==UPD_READY 时才允许调用.
 * 返回 upd_err_t; 成功时 otadata 已指向新槽, 由调用方决定何时重启. */
int update_start(void);

/* URL 字符串上限 (清单地址 = UPD_API + "&device=" + 12 位 MAC ≈ 81 字节;
 * 256 留三倍余量, 也顺带限定了下载地址的长度) */
#define UPDATE_URL_MAX 256

/* 失败原因的中文文案 (不含 HTTP 状态码, UI 自己拼) */
const char *update_err_text(int err);

#ifdef __cplusplus
}
#endif

#endif
