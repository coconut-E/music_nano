/* 网络 OTA: 版本检查 (GitHub Releases API) + 下载 + 分块写备份槽
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
 * PSRAM 只有 2MB. 所以暂存缓冲是 256KB 一块 (UPD_STAGE_BYTES): 读满就停止 read
 * (不关连接、不碰 esp_wifi_*; TCP 窗口关死, 服务端自己会停), 写完这块再继续读.
 *
 * 为什么写 flash 要 4KB 一步 + 每步 vTaskDelay
 * --------------------------------------------
 * 写 flash 的窗口里 cache 是关的, 另一个核取不到指令. 擦一个 4KB 扇区约 25~50ms,
 * 连续擦 2.2MB 就是十几秒的整机卡死 (界面、音频、蓝牙全停). 所以每写 4KB 就让出
 * 一次 CPU, 把停顿切成一片一片, 让别的任务每 ~35ms 能拿到一次执行机会.
 *
 * 为什么不能把 PSRAM 指针直接交给 esp_ota_write
 * ---------------------------------------------
 * ESP32 主 flash (SPI1) 的 supports_direct_write() 恒为 false
 * (components/hal/spi_flash_hal.c:155), 于是 esp_flash_api.c:1128 的 direct_write
 * 为 false, 会退化成"32 字节一次"的 temp_buf 拷贝 —— 2.2MB 就是 6.9 万次 cache
 * 开关, 比整段停一次还糟. 所以先 memcpy 搬进一块 4KB 的内部 RAM
 * (UPD_BOUNCE_BYTES), 从内部 RAM 写走 direct 路径.
 *
 * 为什么用 OTA_WITH_SEQUENTIAL_WRITES
 * -----------------------------------
 * esp_ota_begin() 如果给了具体 image_size, 会先 esp_partition_erase_range() 把整包
 * 擦掉 (esp_ota_ops.c:190) —— 2.2MB 连续擦除, 十几秒不可中断, 正是上面要避免的.
 * 传 OTA_WITH_SEQUENTIAL_WRITES 则改成"写到哪里擦到哪里" (esp_ota_ops.c:314 的
 * need_erase 分支), 擦除跟着 4KB 的步进走.
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

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "nvs.h"

#include "update.h"

static const char *TAG = "UPDATE";

/* ── 服务器 ── */
/* 编译期默认值: gh-proxy 把 api.github.com 和 github.com 都代理了, 国内直连可达;
 * 两个串都存 NVS, 串口 `ota server <api> <dlp>` 可以改 (换镜像/换局域网调试服务器).
 * 局域网调试服务器的 dlp 留空串: 它返回的 browser_download_url 本来就是绝对地址. */
#define UPD_DEF_API   "https://gh-proxy.com/https://api.github.com/repos/coconut-E/music_nano/releases?per_page=5"
#define UPD_DEF_DLP   "https://gh-proxy.com/"

#define UPD_NVS_NS    "ota"
#define UPD_NVS_API   "api"
#define UPD_NVS_DLP   "dlp"

#define UPD_URL_MAX   384
#define UPD_UA_MAX    48

/* GitHub 返回的 JSON 上限. per_page=5、一个 release 连正文也就几 KB,
 * 超过这个数就认为不是我们能认的东西, 直接报错 (不截断解析, 免得解析出半个字段) */
#define UPD_JSON_MAX  (24 * 1024)

/* 暂存块 / 写 flash 的步长 */
#define UPD_STAGE_BYTES   (256 * 1024)
#define UPD_BOUNCE_BYTES  4096
/* 每写 4KB 让出 CPU 的时间. CONFIG_FREERTOS_HZ=1000, 所以 2 就是 2ms ——
 * 一次扇区擦写要 25~50ms, 这个空档让界面能画一帧. 改大只会拖慢总时长. */
#define UPD_STEP_DELAY_MS 2

/* busy 期间电源键无效, 每个网络动作都必须有硬上限, 否则会锁死在退不出去的页面 */
#define UPD_TMO_CONNECT_MS  5000      /* 建连 + TLS 握手 + 收响应头 */
#define UPD_TMO_READ_MS     10000     /* 下载时单次 read 的空闲超时 */
#define UPD_TMO_TOTAL_MS    180000    /* 整个下载+写入的上限 */

/* 证书: 由 CMakeLists 的 EMBED_TXTFILES 打进来 (PEM 末尾自带 NUL).
 * 内容是"本地自签 CA + ISRG Root X1 (+X2)"拼在一起 —— mbedtls 支持一个 buffer
 * 里放多张证书, 所以局域网调试服务器和 gh-proxy 共用一份固件, 换服务器不用重编. */
extern const char ota_ca_pem_start[] asm("_binary_ota_ca_pem_start");

/* ── 状态 ── */

static upd_status_t s_st;

/* 版本查询的结果留到用户点「更新」时用 (update_start 只认 READY) */
static char     s_dl_url[UPD_URL_MAX * 2];
static char     s_sha[65];     /* 服务器给的 sha256 (空串 = 没有/不校验) */
static uint32_t s_expect;      /* 固件总长 (JSON 里的 size) */

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
    case UPD_E_PARSE:       return "返回内容无法解析";
    case UPD_E_NO_REL:      return "还没有发布版本";
    case UPD_E_NO_ASSET:    return "版本里没有固件文件";
    case UPD_E_NOMEM:       return "内存不足";
    case UPD_E_SHORT:       return "下载中断";
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

/* 资产名必须是 music_nano + 一个分隔符 + x.y.z + .bin
 * (发布约定是 "music_nano 0.7.9.bin"; 空格/下划线/短横都认,
 *  因为 URL 里空格会变成 %20, 手滑打成下划线也不至于整个流程失败) */
static bool asset_version(const char *name, char *ver, size_t ver_sz)
{
    static const char *PFX = "music_nano";
    if (strncmp(name, PFX, 10) != 0) return false;

    const char *p = name + 10;
    if (*p != ' ' && *p != '_' && *p != '-') return false;
    p++;

    const char *q = p;
    for (int seg = 0; seg < 3; seg++) {
        const char *d = q;
        while (*d >= '0' && *d <= '9') d++;
        if (d == q) return false;                   /* 每段至少一位数字 */
        if (seg < 2) {
            if (*d != '.') return false;
            d++;
        }
        q = d;
    }
    if (strcmp(q, ".bin") != 0) return false;

    size_t n = (size_t)(q - p);
    if (n >= ver_sz) return false;
    memcpy(ver, p, n);
    ver[n] = '\0';
    return true;
}

/* 在 [from,to) 里找最后一次出现的 needle (资产对象里 name/size 都在
 * browser_download_url 前面, 所以"往前找最后一个"就落在同一个资产对象里) */
static const char *rfind_range(const char *from, const char *to, const char *needle)
{
    size_t n = strlen(needle);
    const char *best = NULL;
    for (const char *p = from; p + n <= to; p++) {
        if (memcmp(p, needle, n) == 0) best = p;
    }
    return best;
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

typedef struct {
    char     ver[24];
    char     url[UPD_URL_MAX];
    uint32_t size;
    char     sha[65];       /* 空串 = 服务器没给 sha256 */
} asset_t;

/* 从 GitHub 的 releases JSON 里挑出"版本最高的 music_nano x.y.z.bin".
 * 这不是通用 JSON 解析器, 只认 GitHub 这一套固定字段顺序:
 *   {..., "name":"music_nano 0.7.9.bin", ..., "size":2200256, ..., "browser_download_url":"..."}
 * 返回 UPD_OK / UPD_E_NO_ASSET */
static int parse_releases(char *json, asset_t *best)
{
    bool any = false;
    char *p = json;

    while ((p = strstr(p, "\"browser_download_url\"")) != NULL) {
        char *q = strchr(p, ':');
        if (!q) break;
        q++;
        while (*q == ' ') q++;
        if (*q != '"') { p++; continue; }

        char url[UPD_URL_MAX];
        if (!take_str(q + 1, url, sizeof url)) { p++; continue; }

        /* 同一个资产对象里往前找 name / size / digest */
        const char *win = (p - json > 2048) ? p - 2048 : json;
        const char *np  = rfind_range(win, p, "\"name\"");
        const char *sp  = rfind_range(win, p, "\"size\"");
        if (!np || !sp) { p++; continue; }

        char *nv = strchr(np, ':');
        if (!nv) { p++; continue; }
        nv++;
        while (*nv == ' ') nv++;
        if (*nv != '"') { p++; continue; }

        char name[96];
        if (!take_str(nv + 1, name, sizeof name)) { p++; continue; }

        char ver[24];
        if (!asset_version(name, ver, sizeof ver)) { p++; continue; }

        char *sd = strchr(sp, ':');
        if (!sd) { p++; continue; }
        uint32_t size = (uint32_t)strtoul(sd + 1, NULL, 10);

        if (!any || ver_cmp(ver, best->ver) > 0) {
            snprintf(best->ver, sizeof best->ver, "%s", ver);
            snprintf(best->url, sizeof best->url, "%s", url);
            best->size = size;
            best->sha[0] = '\0';
            /* digest 是 GitHub 后来加的字段, 老资产可能是 null, 有才用.
             * 只在 "size" 和 "browser_download_url" 之间找: 再往前找会拿到
             * 上一个资产的 digest, 那会把校验搞成"永远失败". 找不到就当没有
             * (esp_ota_end 那一步本来也会验证镜像完整性). */
            const char *dp = rfind_range(sd, p, "\"digest\"");
            if (dp) {
                const char *dv = strchr(dp, ':');
                if (dv) {
                    dv++;
                    while (*dv == ' ') dv++;
                    if (*dv == '"' && strncmp(dv + 1, "sha256:", 7) == 0 &&
                        strlen(dv + 8) >= 64) {
                        memcpy(best->sha, dv + 8, 64);
                        best->sha[64] = '\0';
                    }
                }
            }
            any = true;
        }
        p++;
    }
    return any ? UPD_OK : UPD_E_NO_ASSET;
}

/* ── HTTP ── */

/* 建连 + 发请求头, 不读 body (esp_http_client_perform 会把 body 读完丢掉, 不能用) */
static esp_http_client_handle_t http_open(const char *url, int timeout_ms)
{
    esp_http_client_config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.url        = url;
    cfg.cert_pem   = ota_ca_pem_start;
    cfg.timeout_ms = timeout_ms;
    cfg.buffer_size = 2048;

    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        ESP_LOGE(TAG, "esp_http_client_init 失败");
        return NULL;
    }

    /* GitHub 的 API 对没有 User-Agent 的请求直接 403, esp_http_client 默认不带 */
    char ua[UPD_UA_MAX];
    snprintf(ua, sizeof ua, "music_nano-ota/%s", esp_app_get_description()->version);
    esp_http_client_set_header(c, "User-Agent", ua);
    esp_http_client_set_header(c, "Accept", "application/vnd.github+json");

    esp_err_t e = esp_http_client_open(c, 0);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "打开 %s 失败: %s", url, esp_err_to_name(e));
        esp_http_client_cleanup(c);
        return NULL;
    }
    return c;
}

/* 拉版本 JSON 到 buf. *status 回传 HTTP 状态码 */
static int fetch_json(const char *url, char *buf, size_t cap, size_t *out_len, int *status)
{
    esp_http_client_handle_t c = http_open(url, UPD_TMO_CONNECT_MS);
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

/* ── 服务器地址 (NVS) ── */

/* 读一个字符串键; 键不存在/空间不够返回 false (空串算读到了) */
static bool nvs_read_raw(const char *key, char *out, size_t sz)
{
    nvs_handle_t h;
    if (nvs_open(UPD_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t n = sz;
    esp_err_t e = nvs_get_str(h, key, out, &n);
    nvs_close(h);
    return e == ESP_OK;
}

void update_server_get(char *api, size_t api_sz, char *dlp, size_t dlp_sz)
{
    if (!nvs_read_raw(UPD_NVS_API, api, api_sz)) snprintf(api, api_sz, "%s", UPD_DEF_API);
    /* dlp 允许是空串 (局域网调试服务器的 browser_download_url 本来就是绝对地址),
     * 所以这里只判"键在不在", 不判内容 */
    if (!nvs_read_raw(UPD_NVS_DLP, dlp, dlp_sz)) snprintf(dlp, dlp_sz, "%s", UPD_DEF_DLP);
}

bool update_server_set(const char *api, const char *dlp){
    nvs_handle_t h;
    esp_err_t e = nvs_open(UPD_NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "打开 NVS \"%s\" 失败: %s", UPD_NVS_NS, esp_err_to_name(e));
        return false;
    }
    e = nvs_set_str(h, UPD_NVS_API, api ? api : "");
    if (e == ESP_OK) e = nvs_set_str(h, UPD_NVS_DLP, dlp ? dlp : "");
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "写服务器地址失败: %s", esp_err_to_name(e));
        return false;
    }
    ESP_LOGI(TAG, "服务器地址已更新: api=\"%s\" dlp=\"%s\"", api ? api : "", dlp ? dlp : "");
    return true;
}

/* 抹掉两个键, 回到编译期默认值 (gh-proxy + GitHub Releases) */
bool update_server_reset(void)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(UPD_NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "打开 NVS \"%s\" 失败: %s", UPD_NVS_NS, esp_err_to_name(e));
        return false;
    }
    e = nvs_erase_key(h, UPD_NVS_API);
    if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
    if (e == ESP_OK) {
        esp_err_t e2 = nvs_erase_key(h, UPD_NVS_DLP);
        if (e2 != ESP_OK && e2 != ESP_ERR_NVS_NOT_FOUND) e = e2;
    }
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "恢复默认服务器失败: %s", esp_err_to_name(e));
        return false;
    }
    return true;
}

/* ── 查版本 ── */

void update_reset(void)
{
    memset(&s_st, 0, sizeof s_st);
    s_st.phase = UPD_IDLE;
    snprintf(s_st.cur_ver, sizeof s_st.cur_ver, "%s", esp_app_get_description()->version);
    s_dl_url[0] = '\0';
    s_sha[0]    = '\0';
    s_expect    = 0;
}

int update_check(void)
{
    memset(&s_st, 0, sizeof s_st);
    s_st.phase = UPD_CHECKING;
    snprintf(s_st.cur_ver, sizeof s_st.cur_ver, "%s", esp_app_get_description()->version);
    s_dl_url[0] = '\0';
    s_sha[0]    = '\0';
    s_expect    = 0;

    char api[UPD_URL_MAX], dlp[UPD_URL_MAX];
    update_server_get(api, sizeof api, dlp, sizeof dlp);
    ESP_LOGI(TAG, "查版本: %s", api);
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
    int r = fetch_json(api, json, UPD_JSON_MAX, &len, &http);
    s_st.http = http;
    if (r != UPD_OK) {
        heap_caps_free(json);
        goto fail;
    }
    json[len] = '\0';

    asset_t a;
    memset(&a, 0, sizeof a);
    r = parse_releases(json, &a);
    heap_caps_free(json);
    if (r != UPD_OK) goto fail;
    if (a.size == 0) {
        ESP_LOGE(TAG, "资产 %s 的 size 是 0", a.ver);
        r = UPD_E_PARSE;
        goto fail;
    }

    snprintf(s_st.new_ver, sizeof s_st.new_ver, "%s", a.ver);
    ESP_LOGI(TAG, "本机 %s, 服务器最新 %s (%u B, sha=%s)",
             s_st.cur_ver, a.ver, (unsigned)a.size, a.sha[0] ? "有" : "无");

    if (ver_cmp(a.ver, s_st.cur_ver) <= 0) {
        s_st.phase = UPD_LATEST;
        return UPD_OK;
    }

    /* 拼下载地址: dlp 是镜像前缀 (gh-proxy), 局域网调试服务器时为空串 */
    snprintf(s_dl_url, sizeof s_dl_url, "%s%s", dlp, a.url);
    snprintf(s_sha, sizeof s_sha, "%s", a.sha);
    s_expect   = a.size;
    s_st.total = a.size;
    s_st.phase = UPD_READY;
    ESP_LOGI(TAG, "发现新版本, 下载地址: %s", s_dl_url);
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

/* 返回 upd_err_t; 成功时 otadata 已指向备份槽 */
static int ota_run(const char *url, uint32_t expect, const char *sha_hex)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        ESP_LOGE(TAG, "找不到备份槽 (ota_1)");
        return UPD_E_SWITCH;
    }

    /* 两块缓冲都一次拿好: 中途再抢会撞上碎片化 (和横屏 2KB 中转缓冲同一个教训) */
    uint8_t *stage  = heap_caps_malloc(UPD_STAGE_BYTES, MALLOC_CAP_SPIRAM);
    uint8_t *bounce = heap_caps_malloc(UPD_BOUNCE_BYTES, MALLOC_CAP_INTERNAL);
    if (!stage || !bounce) {
        ESP_LOGE(TAG, "缓冲申请失败: 暂存 %s, 弹跳 %s",
                 stage ? "ok" : "失败", bounce ? "ok" : "失败");
        if (stage)  heap_caps_free(stage);
        if (bounce) heap_caps_free(bounce);
        return UPD_E_NOMEM;
    }

    esp_ota_handle_t h = 0;
    esp_err_t e = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin 失败: %s", esp_err_to_name(e));
        heap_caps_free(stage);
        heap_caps_free(bounce);
        return UPD_E_WRITE;
    }

    esp_http_client_handle_t c = http_open(url, UPD_TMO_READ_MS);
    if (!c) {
        esp_ota_abort(h);
        heap_caps_free(stage);
        heap_caps_free(bounce);
        return UPD_E_NET;
    }

    int64_t clen = esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    s_st.http = status;
    if (status != 200) {
        ESP_LOGE(TAG, "下载返回 HTTP %d", status);
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        esp_ota_abort(h);
        heap_caps_free(stage);
        heap_caps_free(bounce);
        return UPD_E_HTTP;
    }
    if (expect == 0 && clen > 0) {
        expect = (uint32_t)clen;      /* JSON 没给 size 时退而用 Content-Length */
        s_st.total = expect;
    }

    ESP_LOGI(TAG, "开始下载 %u B 到 \"%s\"", (unsigned)expect, part->label);

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);     /* mbedtls 3.x: 这几个就是返回 int 的版本
                                         * (老代码里的 *_ret 已删掉) */

    int64_t t0 = esp_timer_get_time();
    uint32_t off = 0;
    int ret = UPD_OK;
    bool ended = false;

    for (;;) {
        if (expect && off >= expect) break;
        if ((esp_timer_get_time() - t0) / 1000 > UPD_TMO_TOTAL_MS) {
            ESP_LOGE(TAG, "整体超时 (%d 秒)", UPD_TMO_TOTAL_MS / 1000);
            ret = UPD_E_SHORT;
            break;
        }

        /* 1) 收一块进 PSRAM —— 收到想要的就停手, 停手期间 TCP 窗口关死,
         *    服务端自己会停下来等我们 */
        uint32_t want = UPD_STAGE_BYTES;
        if (expect && want > expect - off) want = expect - off;

        size_t got = 0;
        bool eof = false;
        s_st.phase = UPD_DOWNLOADING;
        while (got < want) {
            int r = esp_http_client_read(c, (char *)stage + got, (int)(want - got));
            if (r <= 0) { eof = true; break; }
            mbedtls_sha256_update(&sha, stage + got, (size_t)r);
            got += (size_t)r;
            s_st.dl += (uint32_t)r;
        }
        if (got == 0) break;

        /* 2) 写进 flash: 4KB 一步, 每步让出 CPU */
        s_st.phase = UPD_WRITING;
        for (size_t done = 0; done < got; ) {
            size_t n = got - done;
            if (n > UPD_BOUNCE_BYTES) n = UPD_BOUNCE_BYTES;

            memcpy(bounce, stage + done, n);        /* 此刻 cache 开着, 读 PSRAM 安全 */
            e = esp_ota_write(h, bounce, n);        /* 内部 RAM 源 → direct 路径 */
            if (e != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_write 失败 (off=%u): %s", (unsigned)off, esp_err_to_name(e));
                ret = UPD_E_WRITE;
                break;
            }
            done += n;
            off  += n;
            s_st.wr = off;
            vTaskDelay(pdMS_TO_TICKS(UPD_STEP_DELAY_MS));
        }
        if (ret != UPD_OK) break;
        if (eof) break;
    }

    esp_http_client_close(c);
    esp_http_client_cleanup(c);

    if (ret == UPD_OK && expect && off != expect) {
        ESP_LOGE(TAG, "下载不完整: %u/%u 字节", (unsigned)off, (unsigned)expect);
        ret = UPD_E_SHORT;
    }

    if (ret == UPD_OK) {
        /* 服务器给了 sha256 就先对一遍: 内容不对的话连 esp_ota_end 都不用跑 */
        if (sha_hex && sha_hex[0]) {
            uint8_t dg[32];
            char hex[65];
            mbedtls_sha256_finish(&sha, dg);
            for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", dg[i]);
            if (strcasecmp(hex, sha_hex) != 0) {
                ESP_LOGE(TAG, "sha256 不符: 收到 %s, 服务器说 %s", hex, sha_hex);
                ret = UPD_E_VERIFY;
            }
        }
    }

    if (ret == UPD_OK) {
        e = esp_ota_end(h);         /* 读回整包验证: 约 1 秒, 不关 cache */
        ended = true;
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_end 校验失败: %s", esp_err_to_name(e));
            ret = UPD_E_VERIFY;
        }
    }

    if (!ended) esp_ota_abort(h);

    if (ret == UPD_OK) {
        e = esp_ota_set_boot_partition(part);   /* 唯一一次写 otadata */
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_set_boot_partition 失败: %s", esp_err_to_name(e));
            ret = UPD_E_SWITCH;
        }
    }

    mbedtls_sha256_free(&sha);
    heap_caps_free(stage);
    heap_caps_free(bounce);

    if (ret == UPD_OK) {
        int64_t ms = (esp_timer_get_time() - t0) / 1000;
        ESP_LOGI(TAG, "写入完成: %u B → %s, 耗时 %lld ms (%.0f KB/s), 下次从该槽启动",
                 (unsigned)off, part->label, (long long)ms,
                 ms > 0 ? (double)off / 1024.0 * 1000.0 / (double)ms : 0.0);
    }
    return ret;
}

int update_start(void)
{
    if (s_st.phase != UPD_READY || !s_dl_url[0]) {
        ESP_LOGE(TAG, "没有可升级的版本 (phase=%d)", (int)s_st.phase);
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

    /* 下载地址和 sha256 都是 update_check 时存下来的 */
    int r = ota_run(s_dl_url, s_expect, s_sha);
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
