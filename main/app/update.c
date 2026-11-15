/* 网络 OTA: 查自建服务器的版本清单 + 分段下载 + 分步写备份槽
 *
 * 只由 wifi_mgr 的 worker 任务调用 (见 wifi_mgr.c 的任务模型):
 *   - TLS 握手 + 2.2MB 下载要几十秒, 放 LVGL 任务里界面会死;
 *   - esp_ota_write 会关 flash cache, 必须能主动让出 CPU (见下面"分步写").
 * 本文件不碰界面, 也不碰 esp_wifi_*; 进度只写 s_st, 由 UI 侧轮询.
 *
 * 为什么"下载到 PSRAM 再写 flash"
 * -------------------------------
 * 需求是"下一段 → 暂停下载 → 写进 flash": 写的时候网络上没有半截 TLS 记录要处理,
 * 也不会有 TLS 接收缓冲和 flash cache 互相踩. 但整包放不下 —— 固件 2.2MB, 整颗
 * PSRAM 只有 2MB. 所以暂存缓冲是 64KB 一块 (UPD_STAGE_BYTES): 读满就停止 read
 * (不关连接、不碰 esp_wifi_*; TCP 窗口关死, 服务端自己会停), 写完这块再继续读.
 *
 * 为什么写 flash 要分步 + 每步 vTaskDelay
 * ---------------------------------------
 * 写 flash 的窗口里 cache 是关的, 另一个核取不到指令. 实测 (flashspd 0.7.28):
 * 擦一个 4KB 扇区 17.7ms, 写 4KB ~137ms, 所以 16KB 一步要 ~617ms 全程关 cache.
 * 2.39MB 连续写就是 90 秒的整机卡死 (界面、音频、蓝牙全停). 所以每步之后按
 * 写耗时的 1/9 让出 CPU (见循环里的 rest_ms), 把停顿切成一片一片.
 *
 * 为什么写 flash 要先 memcpy 到 4KB 内部缓冲 ("中转")
 * --------------------------------------------------
 * ESP32 主 flash (SPI1) 的 supports_direct_write() 恒为 false
 * (components/hal/spi_flash_hal.c:155), 于是 esp_flash_api.c:1128 的 direct_write
 * 只取决于源在不在 DRAM:
 *   源在内部 RAM → 一次 MAX_WRITE_CHUNK 传下去;
 *   源在 PSRAM    → 退化成 32 字节一段, 每段关一次 cache 再 memcpy (esp_flash_api.c:1152-1153).
 * 这个差别实测正好是一倍 (flashspd 0.7.28, 每一项都带回读校验):
 *   写 16KB 源内部 RAM   545,644 us (29.3 KB/s)
 *   写 16KB 源 PSRAM   1,130,545 us (14.2 KB/s)   ← 以前 OTA 走的就是这条
 *   擦 16KB (4×4KB 扇区)  70,683 us
 *   线上单步 1.223s = 1,131 (PSRAM 写) + 71 (擦) + 21 (开销), 分毫不差.
 * 所以现在: 下载仍落 PSRAM 暂存 (便宜), 写之前把这一小段搬进 4KB 内部缓冲,
 * 让 esp_flash_write 走快路径。单步 1.223s → ~0.62s, 2.39MB 的写阶段 183s → 92s.
 * 4KB 这个尺寸也是 flashspd 的 C3 项验证过的: 4KB×4 与直接 16KB 同样快, 只多 2.6ms.
 *
 * 以前删掉过这块缓冲, 理由是"下载时 8bit 内部堆只剩 2~3KB, 4KB 申请不到, 申请到
 * 也会饿死 WiFi 收包". 现在条件变了: WiFi 的收包缓冲已搬进私有堆 (app/wifi_arena.c),
 * 公共内部堆在下载开始时实测 27KB / 最大连续块 13KB, 4KB 拿得到.
 *
 * 为什么用 OTA_WITH_SEQUENTIAL_WRITES
 * -----------------------------------
 * esp_ota_begin() 如果给了具体 image_size, 会先 esp_partition_erase_range() 把整包
 * 擦掉 (esp_ota_ops.c:190) —— 2.2MB 连续擦除, 十几秒不可中断, 正是上面要避免的.
 * 传 OTA_WITH_SEQUENTIAL_WRITES 则改成"写到哪里擦到哪里" (esp_ota_ops.c:314 的
 * need_erase 分支), 擦除跟着 4KB 的步进走.
 *
 * 为什么下载分成 5 段 (HTTP Range)
 * --------------------------------
 * 服务器挂在 Cloudflare 隧道后面, 免费层对单个请求有 100 秒的边缘上限. 整包
 * 2.39MB 一次拉完, 平均低于 24 KB/s 就会被半路掐断, 而且断了只能从头再来.
 * 分成 5 段 (~479KB 一段) 后每段约 20 秒, 单段失败只重拉那一段 —— 隧道抖动
 * 时这个差别就是"升得上去"和"永远升不上去".
 *
 * 失败时不动 otadata
 * ------------------
 * 只有最后一步 esp_ota_set_boot_partition() 会写 otadata. 中途任何失败都只留下
 * 备份槽里的一段垃圾, 下次启动照旧跑老槽 —— 和"连接失败直接退出流程"同一个思路:
 * 不搞回滚补偿, 让状态机只有"成功切槽"和"什么都没发生"两种结果. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "miniz.h"          /* ESP32 ROM 自带 tinfl_decompress_mem_to_mem, 无需第三方源码 */

#include "update.h"

static const char *TAG = "UPDATE";

/* ── 服务器 ── */
/* 编译期写死, 不存 NVS, 也没有串口命令能改 —— 换服务器只能改这一行重编一版.
 * 这是有意的: 全项目只有一个发布源 (自建手机 + Cloudflare 隧道), 留一个"可改
 * 地址"的口子, 只会让"设备到底在问谁"变成需要现场排查的问题.
 *
 * 两个 API, 就这两个:
 *   1) 清单:  GET <UPD_API>   → 服务器直接回最新版本那一条 (v1 schema)
 *   2) 固件:  GET <url>       → 带 Range, 分 OTA_BLOCKS 段拉
 *
 * URL 末尾拼 &device=<芯片 MAC>: 服务器只把它回显进响应, 作用是让手机上的
 * ota.log 能对出"是哪台设备在问", 不参与鉴权, 也不影响缓存. */
#define UPD_API \
    "https://coconute.dpdns.org/ota/v1/manifest.json?channel=stable"

#define UPD_URL_MAX   UPDATE_URL_MAX
#define UPD_UA_MAX    48

/* 下载分段数: 5 段 ≈ 479KB/段 (服务器没给 chunks 时的退路) */
#define OTA_BLOCKS    5
/* 服务器把固件切成几块分别 gzip —— 必须和 ota_server.py 的 CHUNKS 一致 */
#define OTA_MAX_CHUNKS 10
/* 单块的上限 (压缩后 / 解压后都按这个申请缓冲). 实际单块约 240KB 原始、
 * 56~196KB 压缩, 320KB 留了余量; 缓冲都在 PSRAM, 两块合计 640KB. */
#define UPD_CHUNK_MAX  (320 * 1024)

/* 清单 JSON 上限. 现在清单里只有一条版本(~400 B), 24KB 是留的余量.
 * 超过这个数就认为不是我们能认的东西, 直接报错 (不截断解析, 免得解析出半个字段) */
#define UPD_JSON_MAX  (24 * 1024)

/* 暂存块 / 写 flash 的步长.
 * STAGE 从 256KB 降到 64KB: 收满一整段就要停下去写 flash, 这期间 TCP 窗口关死,
 * 服务器 sendall 只能干等 —— 实测 256KB 一段会让 2.3MB 的升级反复卡 30~120 秒.
 * 64KB 一段时停顿只有 1/4, 网络和 flash 轮流跑, 总时长反而更短.
 * WRITE_STEP 4KB → 16KB: 每次 esp_ota_write 都要关一次 cache 并判擦除边界,
 * 步长太小等于一直在付固定开销. */
#define UPD_STAGE_BYTES   (64 * 1024)
#define UPD_WRITE_STEP    (16 * 1024)
/* 写 flash 之前的中转块 (内部 RAM). 只要源在 DRAM, esp_flash_write 就走快路径,
 * 与块大小无关; 4KB 是 flashspd 验过的尺寸 (4KB×4 与 16KB×1 同样快, 多付 2.6ms).
 * 选小一点也是为了下载期间别把内部 DMA 池占太多. */
#define UPD_RELAY_BYTES   (4 * 1024)
/* 每步之后最少让出多少毫秒 (写耗时 1/9 ≈ 90% 时间给烧写, 10% 让 CPU 取指令) */
#define UPD_STEP_DELAY_MS 1

/* busy 期间电源键无效, 每个网络动作都必须有硬上限, 否则会锁死在退不出去的页面 */
#define UPD_TMO_CONNECT_MS  5000      /* 建连 + TLS 握手 + 收响应头 */
#define UPD_TMO_READ_MS     20000     /* 下载时单次 read 的空闲超时 (重传退避可能到 24 秒) */
#define UPD_TMO_TOTAL_MS    1800000   /* 整个下载+写入的上限 (30 分钟, 只兜底).
                                       * 实测这条跨境链路随时段在 2~6 KB/s 之间浮动,
                                       * 600 秒会把"慢但能跑完"的升级掐死. */
#define UPD_READ_BUF        2048      /* 下载用的内部缓冲 = 单次 transport read 的上限 */
#define UPD_READ_CHUNK      4096      /* 单次 esp_http_client_read 的长度 */
#define UPD_READ_RETRY      6         /* 连续这么多次读不到数据才放弃 (每次前等 50ms) */

/* 证书: 由 CMakeLists 的 target_add_binary_data 打进来 (PEM 末尾自带 NUL).
 * 内容是"本地自签 CA + ISRG Root X1 (+X2)"拼在一起 —— mbedtls 支持一个 buffer
 * 里放多张证书. 实际用得上的只有 ISRG 那两张: 服务器在 Cloudflare 后面, 边缘证书
 * 由 Let's Encrypt 签, 交叉链里会带上 ISRG Root X2, 所以换域名只要还在 Let's
 * Encrypt 体系里就不用重编固件. */
extern const char ota_ca_pem_start[] asm("_binary_ota_ca_pem_start");

/* ── 状态 ── */

static upd_status_t s_st;

/* 清单里的分块压缩项 (服务器把固件切成 OTA_MAX_CHUNKS 块, 每块单独 gzip).
 * 为什么每块单独压: deflate 是有状态的, 整包压成一个流之后单独取任意一段都解不出
 * 东西; 分成独立小流才能做到"某块失败只重下那一块".
 * size = 压缩后字节数 (实际要下载的), raw = 解压后字节数 (写进 flash 的). */
typedef struct {
    char     url[UPD_URL_MAX];
    uint32_t size;
    uint32_t raw;
} chunk_t;

/* 服务器上的那一份固件 (解析 JSON 的产物, 也会被拷进 upd_target_t) */
typedef struct {
    char     ver[24];
    uint32_t size;          /* 解压后的整包长度 */
    char     sha[65];       /* 空串 = 服务器没给 sha256 */
} asset_t;

/* 版本查询的结果留到用户点「更新」时用 (update_start 只认 READY).
 *
 * 整块放 PSRAM, 而且是"一次申请, 反复用": 里面不但有结果, 还有清单地址、
 * 10 个分块地址和 asset_t. 为什么费这个劲:
 *   1) .bss 每 1 字节都从 DRAM 里扣, 而 DRAM 正是 WiFi 收发缓冲要的池子;
 *   2) 局部数组占的是 **任务栈**, 而这条路径最深的栈就在 mbedtls 握手 —— 之前
 *      把清单地址和解析结果放在 update_check 的栈上 (约 1.1KB), 直接把 wifi 任务的
 *      6KB 栈压爆, 表现为 newlib 建锁时 malloc 失败 → abort (见 locks.c:77).
 * 规则: 给 DMA 用的必须内部 RAM, 只给 CPU 读的大块一律 PSRAM. */
typedef struct {
    char     sha[65];                    /* 服务器给的 sha256 (空串 = 没有/不校验) */
    char     api[UPD_URL_MAX];           /* 清单地址 = UPD_API + "&device=<MAC>" */
    asset_t  asset;                      /* 解析结果 */
    /* 分块压缩清单; chunk_n == 0 表示清单不合规 —— 直接报错, 不保留整包退路 */
    chunk_t  chunk[OTA_MAX_CHUNKS];
    int      chunk_n;
} upd_target_t;

static upd_target_t *s_tgt;     /* PSRAM, update_check() 里申请, update_reset() 释放 */

/* ── 小工具 ── */

const upd_status_t *update_status(void)
{
    return &s_st;
}

const char *update_err_text(int err)
{
    switch (err) {
    case UPD_OK:            return "成功";
    case UPD_E_NET:         return "连不上服务器";
    case UPD_E_HTTP:        return "服务器返回错误";
    case UPD_E_PARSE:       return "清单内容无法解析";
    case UPD_E_NO_REL:      return "服务器上没有这个版本";
    case UPD_E_NO_ASSET:    return "清单里没有下载地址";
    case UPD_E_NOMEM:       return "内存不足";
    case UPD_E_SHORT:       return "下载不完整";
    case UPD_E_WRITE:       return "写入 flash 失败";
    case UPD_E_VERIFY:      return "固件校验失败";
    case UPD_E_SWITCH:      return "切换启动槽失败";
    case UPD_E_VERIFYING:   return "正在校验新固件";
    default:                return "未知错误";
    }
}

/* 三段版本号比较: a<b 返回负, 相等返回 0. 只认数字, 'v' 之类会被跳过,
 * 段数不足按 0 补 (只用于"新的比旧的大"这个判断, 不追求完整语义化版本) */
static int ver_cmp(const char *a, const char *b)
{
    for (int i = 0; i < 3; i++) {
        int va = 0, vb = 0;
        while (*a && (*a < '0' || *a > '9')) a++;
        while (*b && (*b < '0' || *b > '9')) b++;
        while (*a >= '0' && *a <= '9') va = va * 10 + (*a++ - '0');
        while (*b >= '0' && *b <= '9') vb = vb * 10 + (*b++ - '0');
        if (va != vb) return va - vb;
    }
    return 0;
}

/* 取 "key":"value" 里的 value; p 指向开引号之后 */
static bool take_str(const char *p, char *out, size_t out_sz)
{
    const char *e = strchr(p, '"');
    if (!e) return false;
    size_t n = (size_t)(e - p);
    if (n >= out_sz) return false;
    memcpy(out, p, n);
    out[n] = '\0';
    return true;
}

/* 取 "key":"value" 里的 value. key 要连引号一起传 (如 "\"version\""),
 * 否则 "url" 会误命中备注里出现的同名子串. */
static bool json_str(const char *json, const char *key, char *out, size_t out_sz)
{
    const char *p = strstr(json, key);
    if (!p) return false;
    p = strchr(p + strlen(key), ':');
    if (!p) return false;
    p++;
    while (*p == ' ') p++;
    if (*p != '"') return false;
    return take_str(p + 1, out, out_sz);
}

/* 自建服务器的清单 (v1 schema, 见 docs/OTA_TEST_zh.md):
 *   {"schema":1,"channel":"stable","version":"0.8.0","size":2396448,
 *    "sha256":"<64 hex>","url":"https://…/firmware.bin","notes":"…",
 *    "released_at":"…","min_bootloader":"","device":"A0B1C2D3E4F5"}
 *
 * 不是通用 JSON 解析器: 只认这五个键, 顺序无关 (各取第一次出现的).
 * schema 必须是 1 —— 它同时是协议的握手位: 万一哪天升到 2, 这里明确报错,
 * 而不是把新字段误读成旧语义.
 * 返回 UPD_OK / UPD_E_NO_ASSET (没有 url) / UPD_E_PARSE */
static int parse_manifest_v1(const char *json, asset_t *out)
{
    const char *p = strstr(json, "\"schema\"");
    if (!p) return UPD_E_NO_ASSET;          /* 不是自家格式 */
    const char *c = strchr(p, ':');
    if (!c) return UPD_E_PARSE;
    if (strtoul(c + 1, NULL, 10) != 1) {
        ESP_LOGE(TAG, "清单 schema 不是 1");
        return UPD_E_PARSE;
    }

    if (!json_str(json, "\"version\"", out->ver, sizeof out->ver)) return UPD_E_PARSE;

    /* sha256 是裸 64 位 hex; 长度不对就当服务器没给, 那一轮只靠 esp_ota_end 验镜像 */
    out->sha[0] = '\0';
    json_str(json, "\"sha256\"", out->sha, sizeof out->sha);
    if (out->sha[0] && strlen(out->sha) != 64) out->sha[0] = '\0';

    const char *z = strstr(json, "\"size\"");
    if (!z) return UPD_E_PARSE;
    z = strchr(z, ':');
    if (!z) return UPD_E_PARSE;
    out->size = (uint32_t)strtoul(z + 1, NULL, 10);
    return UPD_OK;
}

/* 解析清单里的 "chunks" 数组:
 *   [{"url":"…/c0.gz","size":56359,"raw":239670}, …]
 * 返回解析到的块数, 0 表示服务器没给分块清单 (那就走整包 Range 的老路径).
 *
 * 三者的关系: size 是压缩后要下载的字节数, raw 是解压后写进 flash 的字节数,
 * 全部 raw 之和应当等于清单顶层的 size. */
static int parse_chunks(const char *json, upd_target_t *t)
{
    const char *p = strstr(json, "\"chunks\"");
    if (!p) return 0;

    int n = 0;
    while (n < OTA_MAX_CHUNKS) {
        const char *u = strstr(p, "\"url\"");
        if (!u) break;

        char url[UPD_URL_MAX];
        if (!json_str(u, "\"url\"", url, sizeof url)) break;

        /* size / raw 都在这一块的 url 之后、下一块的 url 之前 */
        const char *sz = strstr(u, "\"size\"");
        const char *rw = strstr(u, "\"raw\"");
        if (!sz || !rw) break;

        const char *c = strchr(sz, ':');
        if (!c) break;
        uint32_t size = (uint32_t)strtoul(c + 1, NULL, 10);

        c = strchr(rw, ':');
        if (!c) break;
        uint32_t raw = (uint32_t)strtoul(c + 1, NULL, 10);

        if (size == 0 || raw == 0 || size > UPD_CHUNK_MAX || raw > UPD_CHUNK_MAX) {
            ESP_LOGW(TAG, "第 %d 块尺寸异常 (gz=%u raw=%u), 忽略分块清单", n, size, raw);
            return 0;
        }

        snprintf(t->chunk[n].url, sizeof t->chunk[n].url, "%s", url);
        t->chunk[n].size = size;
        t->chunk[n].raw  = raw;
        n++;

        p = rw + 5;         /* 从 raw 之后继续找下一块 */
    }
    return n;
}

/* ── HTTP ── */
/* 建连 + 发请求头, 不读 body (esp_http_client_perform 会把 body 读完丢掉, 不能用).
 * buf 是内部缓冲大小: 版本查询用 1KB (省内存), 下载用 2KB —— 它同时也是单次
 * esp_transport_read 的上限.
 *
 * 现在只有两种请求: 拉清单 (https) 和 拉一个压缩块 (http). 两者都不需要 Range ——
 * 分块是服务器切好的独立文件, 不是对一个大文件取区间. */
static esp_http_client_handle_t http_open(const char *url, int timeout_ms, int buf)
{
    esp_http_client_config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.url        = url;
    cfg.cert_pem   = ota_ca_pem_start;     /* 只有 https 用得上; http 会被忽略 */
    cfg.timeout_ms = timeout_ms;
    cfg.buffer_size = buf;

    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        ESP_LOGE(TAG, "esp_http_client_init 失败");
        return NULL;
    }

    char ua[UPD_UA_MAX];
    snprintf(ua, sizeof ua, "music_nano-ota/%s", esp_app_get_description()->version);
    esp_http_client_set_header(c, "User-Agent", ua);

    /* 握手前先量一次栈余量: 这条路径最深的一段在 mbedtls 里, 曾经把 worker 的
     * 6KB 栈压爆 (newlib 建锁时 malloc 失败 → abort, locks.c:77). 这两个数字
     * 以后就是"TLS 到底需要多少栈"的依据. */
    ESP_LOGI(TAG, "建连前栈余量 %u B", (unsigned)uxTaskGetStackHighWaterMark(NULL));

    /* 探针: 自己先要一个 socket 再关掉. 它能把 esp-tls 那句 "Failed to create
     * socket" 的两种原因分开 —— 探针也失败 = lwIP 侧真没资源 (EMFILE 槽位耗尽 /
     * ENOMEM 内部 8bit 内存不够); 探针成功而 http 失败 = 问题在 esp-tls 的建连流程. */
    int probe = socket(AF_INET, SOCK_STREAM, 0);
    if (probe < 0) {
        ESP_LOGE(TAG, "socket 探针失败: errno=%d (EMFILE=%d ENOMEM=%d)",
                 errno, EMFILE, ENOMEM);
    } else {
        ESP_LOGI(TAG, "socket 探针正常 (fd=%d)", probe);
        close(probe);
    }

    esp_err_t e = esp_http_client_open(c, 0);
    if (e != ESP_OK) {
        /* 失败原因基本就两种, 靠 errno 区分:
         *   ENOMEM  → socket() 内部要建的 netconn 收件箱/信号量拿不到内部 8bit 内存
         *             (TLS 缓冲可以放 PSRAM, 这几个小对象不行);
         *   EMFILE  → 10 个 socket 槽用完了 (漏了), 见下面显式 close.
         * 所以这里把 errno 和两组堆数字一起打出来, 一次上机就能定性. */
        ESP_LOGE(TAG, "打开 %s 失败: %s, errno=%d, 8bit内部 %u/最大块 %u, DMA %u/最大块 %u",
                 url, esp_err_to_name(e), errno,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
        /* esp_http_client_close() 只在 state>INIT 时才关 fd (esp_http_client.c),
         * 而 connect 失败时 state 还停在 INIT —— 它不会关. 这里照样先调一次
         * (成功连接后走到这儿的路径需要它), 再 destroy 保证把 socket 收干净. */
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return NULL;
    }
    /* 建连后的内存才是关键数字: 拉清单那次走 https, 握手期间证书解析会临时占
     * 内部 RAM; 下载那 10 块走 http, 不握手. 之前只打"下载前"的数字, 看不到
     * 这一段掉多少 —— 而 lwIP 分 pbuf 正是在这之后. */
    ESP_LOGI(TAG, "建连完成, 栈余量 %u B, 内部 %u (最大块 %u) DMA %u (最大块 %u)",
             (unsigned)uxTaskGetStackHighWaterMark(NULL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    return c;
}

/* 拉版本 JSON 到 buf. *status 回传 HTTP 状态码 */
static int fetch_json(const char *url, char *buf, size_t cap, size_t *out_len, int *status)
{
    esp_http_client_handle_t c = http_open(url, UPD_TMO_CONNECT_MS, 1024);
    if (!c) return UPD_E_NET;

    int64_t len = esp_http_client_fetch_headers(c);
    *status = esp_http_client_get_status_code(c);

    int ret = UPD_OK;
    size_t got = 0;

    if (*status != 200) {
        ESP_LOGE(TAG, "版本接口返回 HTTP %d", *status);
        ret = (*status == 404) ? UPD_E_NO_REL : UPD_E_HTTP;
        goto done;
    }
    if (len > (int64_t)cap - 1) {
        ESP_LOGE(TAG, "版本接口返回 %lld 字节, 超过 %u 上限", (long long)len, (unsigned)cap);
        ret = UPD_E_PARSE;
        goto done;
    }

    while (got < cap - 1) {
        int r = esp_http_client_read(c, buf + got, (int)(cap - 1 - got));
        if (r <= 0) break;
        got += (size_t)r;
        if (len > 0 && got >= (size_t)len) break;   /* 按 Content-Length 收够了 */
        if (esp_http_client_is_complete_data_received(c)) break;
    }
    if (len > 0 && got != (size_t)len) {
        ESP_LOGE(TAG, "版本接口只收到 %u/%lld 字节", (unsigned)got, (long long)len);
        ret = UPD_E_NET;
        goto done;
    }
    *out_len = got;

done:
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return ret;
}

/* ── 查版本 ── */

void update_reset(void)
{
    memset(&s_st, 0, sizeof s_st);
    s_st.phase = UPD_IDLE;
    snprintf(s_st.cur_ver, sizeof s_st.cur_ver, "%s", esp_app_get_description()->version);
    if (s_tgt) { heap_caps_free(s_tgt); s_tgt = NULL; }
}

int update_check(void)
{
    memset(&s_st, 0, sizeof s_st);
    s_st.phase = UPD_CHECKING;
    snprintf(s_st.cur_ver, sizeof s_st.cur_ver, "%s", esp_app_get_description()->version);

    /* 上一次流程可能留着一份 (正常 update_reset 已释放), 先清掉 */
    if (s_tgt) { heap_caps_free(s_tgt); s_tgt = NULL; }
    s_tgt = heap_caps_malloc(sizeof *s_tgt, MALLOC_CAP_SPIRAM);
    if (!s_tgt) {
        ESP_LOGE(TAG, "PSRAM 申请下载目标失败 (%u B)", (unsigned)sizeof *s_tgt);
        goto fail_nomem;
    }
    s_tgt->sha[0] = '\0';

    /* 清单地址 = 编译期常量 + 芯片号. 拼好的串落在 PSRAM 的 s_tgt 里: 它要是放
     * 栈上, 这条路径最深的栈 (mbedtls 握手) 会被多压一百多字节 —— 之前就是那样 abort 的. */
    uint8_t mac[6] = {0};
    char    dev[13];
    esp_efuse_mac_get_default(mac);
    snprintf(dev, sizeof dev, "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(s_tgt->api, sizeof s_tgt->api, "%s&device=%s", UPD_API, dev);
    ESP_LOGI(TAG, "查版本: %s", s_tgt->api);
    ESP_LOGI(TAG, "内存: PSRAM 空闲 %u B, 内部空闲 %u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    char *json = heap_caps_malloc(UPD_JSON_MAX, MALLOC_CAP_SPIRAM);
    if (!json) {
        ESP_LOGE(TAG, "PSRAM 缓冲申请失败 (%u B)", (unsigned)UPD_JSON_MAX);
        goto fail_nomem;
    }

    size_t len = 0;
    int http = 0;
    int r = fetch_json(s_tgt->api, json, UPD_JSON_MAX, &len, &http);
    s_st.http = http;
    if (r != UPD_OK) {
        heap_caps_free(json);
        goto fail;
    }
    json[len] = '\0';

    asset_t *a = &s_tgt->asset;
    memset(a, 0, sizeof *a);
    r = parse_manifest_v1(json, a);
    if (r == UPD_OK) {
        s_tgt->chunk_n = parse_chunks(json, s_tgt);
        if (s_tgt->chunk_n == 0) {
            /* 没有 chunks 就是不认这份清单: 现在不保留"整包直接下"的退路 */
            r = UPD_E_PARSE;
        } else {
            ESP_LOGI(TAG, "分块清单: %d 块", s_tgt->chunk_n);
        }
    }
    heap_caps_free(json);
    if (r != UPD_OK) goto fail;
    if (a->size == 0) {
        ESP_LOGE(TAG, "资产 %s 的 size 是 0", a->ver);
        r = UPD_E_PARSE;
        goto fail;
    }

    snprintf(s_st.new_ver, sizeof s_st.new_ver, "%s", a->ver);
    ESP_LOGI(TAG, "本机 %s, 服务器最新 %s (%u B, sha=%s)",
             s_st.cur_ver, a->ver, (unsigned)a->size, a->sha[0] ? "有" : "无");

    if (ver_cmp(a->ver, s_st.cur_ver) <= 0) {
        s_st.phase = UPD_LATEST;
        return UPD_OK;
    }

    snprintf(s_tgt->sha, sizeof s_tgt->sha, "%s", a->sha);
    s_st.total = a->size;
    s_st.phase = UPD_READY;
    ESP_LOGI(TAG, "发现新版本, 共 %d 块", s_tgt->chunk_n);
    return UPD_OK;

fail_nomem:
    r = UPD_E_NOMEM;
fail:
    s_st.err   = r;
    s_st.phase = UPD_FAIL;
    ESP_LOGE(TAG, "查版本失败: %s (%d, http=%d)", update_err_text(r), r, s_st.http);
    return r;
}

/* ── 下载 + 写 flash ── */

/* 下载一块的压缩数据到 buf. 收满 ck->size 才算成功. */
static int fetch_chunk(const chunk_t *ck, uint8_t *buf, size_t *out_len)
{
    esp_http_client_handle_t c = http_open(ck->url, UPD_TMO_READ_MS, UPD_READ_BUF);
    if (!c) return UPD_E_NET;

    int64_t clen   = esp_http_client_fetch_headers(c);
    int     status = esp_http_client_get_status_code(c);
    s_st.http = status;

    int ret = UPD_OK;
    if (status != 200) {
        ESP_LOGE(TAG, "块返回 HTTP %d", status);
        ret = (status == 404) ? UPD_E_NO_ASSET : UPD_E_HTTP;
        goto done;
    }
    if (clen > 0 && (uint64_t)clen != (uint64_t)ck->size) {
        ESP_LOGE(TAG, "块长度不符: 头部 %lld, 清单 %u", (long long)clen, (unsigned)ck->size);
        ret = UPD_E_SHORT;
        goto done;
    }

    size_t got = 0;
    int    empty = 0;
    s_st.phase = UPD_DOWNLOADING;
    while (got < ck->size) {
        size_t ask = ck->size - got;
        if (ask > UPD_READ_CHUNK) ask = UPD_READ_CHUNK;

        int r = esp_http_client_read(c, (char *)buf + got, (int)ask);
        if (r > 0) {
            got += (size_t)r;
            empty = 0;
            continue;
        }
        if (esp_http_client_is_complete_data_received(c)) break;
        if (++empty >= UPD_READ_RETRY) { ret = UPD_E_NET; break; }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (ret == UPD_OK && got != ck->size) {
        ESP_LOGE(TAG, "块只收到 %u/%u", (unsigned)got, (unsigned)ck->size);
        ret = UPD_E_SHORT;
    }
    *out_len = got;

done:
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return ret;
}

/* 分块压缩下载: 逐块 GET → tinfl 解压 → 写 flash.
 *
 * 比整包 Range 好的地方:
 *   1) 传输量只有 62% (实测 2.40MB → 1.48MB), 在 2~6KB/s 的链路上直接砍掉四成时间;
 *   2) 每块都是独立 gzip 流(约 150KB) —— 单块失败只重下这一块, 不用回退整包.
 *      这是"整包压成一个流"做不到的: deflate 有状态, 从任意字节切开的片段解不出来.
 *
 * sha256 对**解压后**的字节流式累加 —— 那正是最终写进 flash 的内容, 和清单顶层的
 * sha256 是同一份东西. 块内先校验解压长度, 长度不对就不进 sha、不写 flash. */
static int ota_run(upd_target_t *t)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        ESP_LOGE(TAG, "找不到备份槽 (ota_1)");
        return UPD_E_SWITCH;
    }

    uint8_t *cbuf = heap_caps_malloc(UPD_CHUNK_MAX, MALLOC_CAP_SPIRAM);   /* 压缩数据 */
    uint8_t *rbuf = heap_caps_malloc(UPD_CHUNK_MAX, MALLOC_CAP_SPIRAM);   /* 解压结果 */
    if (!cbuf || !rbuf) {
        ESP_LOGE(TAG, "PSRAM 缓冲申请失败 (%u B × 2)", (unsigned)UPD_CHUNK_MAX);
        heap_caps_free(cbuf);
        heap_caps_free(rbuf);
        return UPD_E_NOMEM;
    }
    /* 解压器里有 32KB 的字典窗口, 必须放 PSRAM —— 见下面解压那段 */
    tinfl_decompressor *dec = heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_SPIRAM);
    if (!dec) {
        ESP_LOGE(TAG, "解压器申请失败 (%u B)", (unsigned)sizeof(tinfl_decompressor));
        heap_caps_free(rbuf);
        heap_caps_free(cbuf);
        return UPD_E_NOMEM;
    }
    uint8_t *mid = heap_caps_malloc(UPD_RELAY_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!mid) {
        ESP_LOGW(TAG, "中转缓冲申请失败 (内部 %u B), 写 flash 走 PSRAM 慢路径",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }

    esp_ota_handle_t h = 0;
    esp_err_t e = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin 失败: %s", esp_err_to_name(e));
        heap_caps_free(dec);
        heap_caps_free(mid);
        heap_caps_free(rbuf);
        heap_caps_free(cbuf);
        return UPD_E_WRITE;
    }

    uint32_t gz_total = 0;
    for (int i = 0; i < t->chunk_n; i++) gz_total += t->chunk[i].size;
    ESP_LOGI(TAG, "开始分块下载: %u B → %u B (压缩 %.0f%%), 共 %d 块",
             (unsigned)t->asset.size, (unsigned)gz_total,
             t->asset.size ? (double)gz_total * 100.0 / (double)t->asset.size : 0.0,
             t->chunk_n);

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);

    int64_t  t0  = esp_timer_get_time();
    uint32_t off = 0;
    int      ret = UPD_OK;
    bool     ended = false;

    for (int i = 0; i < t->chunk_n && ret == UPD_OK; i++) {
        const chunk_t *ck = &t->chunk[i];

        if ((esp_timer_get_time() - t0) / 1000 > UPD_TMO_TOTAL_MS) {
            ESP_LOGE(TAG, "整体超时 (%d 秒)", UPD_TMO_TOTAL_MS / 1000);
            ret = UPD_E_SHORT;
            break;
        }

        /* 1) 下载压缩数据, 失败就重下这一块 (最多 3 次) */
        size_t got = 0;
        for (int attempt = 1; ; attempt++) {
            ret = fetch_chunk(ck, cbuf, &got);
            if (ret == UPD_OK) break;
            if (attempt >= 3) break;
            ESP_LOGW(TAG, "第 %d/%d 块失败 (%d), 2 秒后重试 (%d/3)",
                     i + 1, t->chunk_n, ret, attempt);
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
        if (ret != UPD_OK) break;

        /* 2) 解压. 用 ROM 里的 tinfl (ESP32 的 ROM 只带这个解压器).
         * 服务器发的是 zlib 包装 (0x78 0x9c) —— ROM 版 miniz 只有
         * TINFL_FLAG_PARSE_ZLIB_HEADER, 没有 HAS_GZIP_HEADER.
         *
         * 绝对不能用 tinfl_decompress_mem_to_mem: 它把 tinfl_decompressor 放在
         * **栈**上, 那个结构带 32KB 字典窗口, 8KB 的 wifi 任务栈会当场溢出
         * (实测就是 "A stack overflow in task wifi") . 自己申请一个放 PSRAM,
         * 循环外分配、每块 tinfl_init 重置. */
        tinfl_init(dec);
        size_t in_len  = got;
        size_t out_len = UPD_CHUNK_MAX;
        tinfl_status ts = tinfl_decompress(dec, cbuf, &in_len, rbuf, rbuf, &out_len,
                                           TINFL_FLAG_PARSE_ZLIB_HEADER |
                                           TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
        if (ts != TINFL_STATUS_DONE || out_len != ck->raw) {
            ESP_LOGE(TAG, "第 %d/%d 块解压失败 (status=%d, 得到 %u, 清单说 %u)",
                     i + 1, t->chunk_n, (int)ts, (unsigned)out_len, (unsigned)ck->raw);
            ret = UPD_E_VERIFY;
            break;
        }
        size_t out = out_len;

        /* 3) 写 flash + 累加 sha */
        mbedtls_sha256_update(&sha, rbuf, out);
        s_st.phase = UPD_WRITING;
        for (size_t done = 0; done < out; ) {
            size_t n = out - done;
            if (n > UPD_WRITE_STEP) n = UPD_WRITE_STEP;

            int64_t ts = esp_timer_get_time();
            if (mid) {
                e = ESP_OK;
                for (size_t o = 0; o < n && e == ESP_OK; o += UPD_RELAY_BYTES) {
                    size_t m = n - o;
                    if (m > UPD_RELAY_BYTES) m = UPD_RELAY_BYTES;
                    memcpy(mid, rbuf + done + o, m);
                    e = esp_ota_write(h, mid, m);
                }
            } else {
                e = esp_ota_write(h, rbuf + done, n);
            }
            int64_t tw = esp_timer_get_time() - ts;

            if (e != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_write 失败 (off=%u): %s", (unsigned)off, esp_err_to_name(e));
                ret = UPD_E_WRITE;
                break;
            }
            done += n;
            off  += n;
            s_st.wr = off;
            s_st.dl = off;      /* 进度按"已写入"算: dl 是压缩量, 两者同步才不会让界面跳 */

            uint32_t rest_ms = (uint32_t)(tw / 9000);
            if (rest_ms < UPD_STEP_DELAY_MS) rest_ms = UPD_STEP_DELAY_MS;
            vTaskDelay(pdMS_TO_TICKS(rest_ms));
        }
        if (ret != UPD_OK) break;

        int64_t el = (esp_timer_get_time() - t0) / 1000;
        ESP_LOGI(TAG, "第 %d/%d 块完成: 已写 %u/%u B, %lld ms (%.1f KB/s)",
                 i + 1, t->chunk_n, (unsigned)off, (unsigned)t->asset.size,
                 (long long)el, el > 0 ? (double)off / 1024.0 * 1000.0 / (double)el : 0.0);
    }

    if (ret == UPD_OK && off != t->asset.size) {
        ESP_LOGE(TAG, "解压总量不符: %u/%u", (unsigned)off, (unsigned)t->asset.size);
        ret = UPD_E_SHORT;
    }

    if (ret == UPD_OK && t->sha[0]) {
        uint8_t dg[32];
        char hex[65];
        mbedtls_sha256_finish(&sha, dg);
        for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", dg[i]);
        if (strcasecmp(hex, t->sha) != 0) {
            ESP_LOGE(TAG, "sha256 不符: 收到 %s, 服务器说 %s", hex, t->sha);
            ret = UPD_E_VERIFY;
        }
    }

    if (ret == UPD_OK) {
        e = esp_ota_end(h);
        ended = true;
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_end 校验失败: %s", esp_err_to_name(e));
            ret = UPD_E_VERIFY;
        }
    }
    if (!ended) esp_ota_abort(h);

    if (ret == UPD_OK) {
        e = esp_ota_set_boot_partition(part);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_set_boot_partition 失败: %s", esp_err_to_name(e));
            ret = UPD_E_SWITCH;
        }
    }

    mbedtls_sha256_free(&sha);
    heap_caps_free(dec);
    heap_caps_free(mid);
    heap_caps_free(rbuf);
    heap_caps_free(cbuf);

    if (ret == UPD_OK) {
        int64_t ms = (esp_timer_get_time() - t0) / 1000;
        ESP_LOGI(TAG, "写入完成: %u B → %s, 耗时 %lld ms (%.1f KB/s), 下次从该槽启动",
                 (unsigned)off, part->label, (long long)ms,
                 ms > 0 ? (double)off / 1024.0 * 1000.0 / (double)ms : 0.0);
    }
    return ret;
}

int update_start(void)
{
    if (s_st.phase != UPD_READY || !s_tgt || s_tgt->chunk_n <= 0) {
        ESP_LOGE(TAG, "没有可升级的版本 (phase=%d, chunk_n=%d)",
                 (int)s_st.phase, s_tgt ? s_tgt->chunk_n : -1);
        return UPD_E_PARSE;
    }

    /* 运行槽必须已确认 (VALID). 如果它还是 PENDING_VERIFY, 说明"新固件首启的
     * 校验窗口"还没走完, 这时把启动项切走, 那条 pending 记录会留在另一个
     * otadata 扇区里, 万一新固件崩溃回退到它就会被判 ABORTED. 实际进不来
     * (窗口期有全屏遮罩), 但挡住比事后解释便宜. */
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK && st != ESP_OTA_IMG_VALID) {
        ESP_LOGE(TAG, "运行槽状态是 %d (不是 VALID), 暂不升级", (int)st);
        s_st.err = UPD_E_VERIFYING;
        s_st.phase = UPD_FAIL;
        return UPD_E_VERIFYING;
    }

    ESP_LOGI(TAG, "开始升级: %s → %s", s_st.cur_ver, s_st.new_ver);
    s_st.phase = UPD_DOWNLOADING;
    s_st.err   = 0;
    s_st.dl    = 0;
    s_st.wr    = 0;

    int r = ota_run(s_tgt);
    ESP_LOGI(TAG, "任务栈余量 %u B", (unsigned)uxTaskGetStackHighWaterMark(NULL));

    if (r == UPD_OK) {
        s_st.phase = UPD_DONE;
        return UPD_OK;
    }
    s_st.err   = r;
    s_st.phase = UPD_FAIL;
    ESP_LOGE(TAG, "升级失败: %s (%d, http=%d)", update_err_text(r), r, s_st.http);
    return r;
}
