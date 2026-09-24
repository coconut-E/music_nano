#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "likes.h"

static const char *TAG = "LIKES";

#define LIKES_NS          "likes"
#define LIKES_BUCKETS     10        /* 键 "0".."9" */
#define GROUP_HASHES      16        /* 每个完整组 16 个 uint32 */
#define SEP_BYTES         8         /* 组间分隔符: 64 bit 全零 */
#define MAX_BUCKET_HASHES 4096      /* 单桶哈希上限 (保护性) */
#define MAX_BUCKET_BYTES  (MAX_BUCKET_HASHES * 4 + (MAX_BUCKET_HASHES / GROUP_HASHES) * SEP_BYTES)

static nvs_handle_t s_nvs;
static bool         s_open = false;

/* RAM 缓存: 全部已喜欢哈希 (init 时从 10 个桶载入, add/remove 同步维护) */
static uint32_t *s_cache     = NULL;
static int       s_cache_n   = 0;
static int       s_cache_cap = 0;

/* 小端读写 (与端序无关) */
static inline uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void write_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* PSRAM 优先分配, 失败回退内部 RAM */
static void *likes_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_malloc(n, MALLOC_CAP_8BIT);
    return p;
}

/* 追加一段哈希 (段长已保证为 4 的整数倍) */
static void append_segment(const uint8_t *seg, size_t seglen,
                           uint32_t *out, int *n, int out_max)
{
    size_t cnt = seglen / 4;
    for (size_t i = 0; i < cnt; i++) {
        if (*n >= out_max) return;
        out[(*n)++] = read_le32(seg + i * 4);
    }
}

/*
 * 解析桶: 以 8 字节全零为分隔符切段, 逐段校验.
 *   - 中间段 (两侧都有分隔符): 必须恰好 16 个哈希 (64 字节)
 *   - 头/尾段 (仅一侧或没有分隔符): 允许不满 16 个, 但须为 4 字节 (32 bit) 整数倍
 * 不合法的段只丢弃该段本身, 其它段照常保留.
 * 返回收集到的哈希个数.
 */
static int parse_bucket(const uint8_t *buf, size_t len, uint32_t *out, int out_max)
{
    int n = 0;
    size_t seg_start = 0;
    bool left_bounded = false;   /* 当前段左侧是否已有分隔符 */
    size_t i = 0;

    while (i < len) {
        bool is_sep = false;
        if (i + SEP_BYTES <= len) {
            is_sep = true;
            for (int k = 0; k < SEP_BYTES; k++) {
                if (buf[i + k] != 0) { is_sep = false; break; }
            }
        }

        if (is_sep) {
            size_t seglen = i - seg_start;
            if (seglen > 0) {
                bool middle = left_bounded;
                if (seglen % 4 == 0 && (!middle || seglen == (size_t)GROUP_HASHES * 4)) {
                    append_segment(buf + seg_start, seglen, out, &n, out_max);
                } else {
                    ESP_LOGW(TAG, "段非法 (%zu 字节, %s), 仅丢弃该段",
                             seglen, middle ? "中段" : "头/尾");
                }
            }
            i += SEP_BYTES;
            seg_start = i;
            left_bounded = true;
        } else {
            i++;
        }
    }

    /* 尾段 (右侧无分隔符) */
    if (len > seg_start) {
        size_t seglen = len - seg_start;
        if (seglen % 4 == 0) {
            append_segment(buf + seg_start, seglen, out, &n, out_max);
        } else {
            ESP_LOGW(TAG, "尾段非法 (%zu 字节), 仅丢弃该段", seglen);
        }
    }
    return n;
}

/* 序列化: 返回写入字节数 (分隔符只加在完整组之间, 尾部不加) */
static size_t serialize_bucket(const uint32_t *hashes, int n, uint8_t *out)
{
    size_t off = 0;
    for (int i = 0; i < n; i++) {
        if (i > 0 && i % GROUP_HASHES == 0) {
            memset(out + off, 0, SEP_BYTES);
            off += SEP_BYTES;
        }
        write_le32(out + off, hashes[i]);
        off += 4;
    }
    return off;
}

/* 读取桶到 out, 返回个数 (0 = 空/无/非法) */
static int load_bucket(int b, uint32_t *out, int out_max)
{
    if (!s_open) return 0;

    char key[2] = { (char)('0' + b), '\0' };
    size_t len = 0;
    if (nvs_get_blob(s_nvs, key, NULL, &len) != ESP_OK || len == 0) return 0;
    if (len > MAX_BUCKET_BYTES) {
        ESP_LOGW(TAG, "桶 %d 过大 (%zu), 丢弃", b, len);
        return 0;
    }

    uint8_t *buf = likes_alloc(len);
    if (!buf) return 0;

    size_t rlen = len;
    esp_err_t r = nvs_get_blob(s_nvs, key, buf, &rlen);
    int n = 0;
    if (r == ESP_OK) {
        n = parse_bucket(buf, rlen, out, out_max);
    }
    heap_caps_free(buf);
    return n;
}

/* 写回桶 (n<=0 则删除该键) */
static void save_bucket(int b, const uint32_t *hashes, int n)
{
    if (!s_open) return;

    char key[2] = { (char)('0' + b), '\0' };

    if (n <= 0) {
        nvs_erase_key(s_nvs, key);
        nvs_commit(s_nvs);
        return;
    }

    size_t len = (size_t)n * 4 + (size_t)((n - 1) / GROUP_HASHES) * SEP_BYTES;
    uint8_t *buf = likes_alloc(len);
    if (!buf) return;

    serialize_bucket(hashes, n, buf);
    esp_err_t r = nvs_set_blob(s_nvs, key, buf, len);
    if (r == ESP_OK) r = nvs_commit(s_nvs);
    if (r != ESP_OK) ESP_LOGW(TAG, "写桶 %d 失败: %s", b, esp_err_to_name(r));

    heap_caps_free(buf);
}

/* ── RAM 缓存操作 ── */

static bool cache_contains(uint32_t h)
{
    for (int i = 0; i < s_cache_n; i++) {
        if (s_cache[i] == h) return true;
    }
    return false;
}

static bool cache_push(uint32_t h)
{
    if (s_cache_n == s_cache_cap) {
        int nc = s_cache_cap ? s_cache_cap * 2 : 256;
        uint32_t *na = likes_alloc((size_t)nc * sizeof(uint32_t));
        if (!na) return false;
        if (s_cache) {
            memcpy(na, s_cache, (size_t)s_cache_n * sizeof(uint32_t));
            heap_caps_free(s_cache);
        }
        s_cache = na;
        s_cache_cap = nc;
    }
    s_cache[s_cache_n++] = h;
    return true;
}

static void cache_remove_at(int idx)
{
    for (int i = idx; i < s_cache_n - 1; i++) s_cache[i] = s_cache[i + 1];
    s_cache_n--;
}

/* 由 RAM 缓存重写某个桶 */
static void save_bucket_from_cache(int b)
{
    uint32_t *tmp = likes_alloc(MAX_BUCKET_HASHES * sizeof(uint32_t));
    if (!tmp) return;
    int n = 0;
    for (int i = 0; i < s_cache_n && n < MAX_BUCKET_HASHES; i++) {
        if ((int)(s_cache[i] % LIKES_BUCKETS) == b) tmp[n++] = s_cache[i];
    }
    save_bucket(b, tmp, n);
    heap_caps_free(tmp);
}

void likes_init(void)
{
    if (s_open) return;

    if (nvs_open(LIKES_NS, NVS_READWRITE, &s_nvs) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open(%s) 失败, 喜欢功能禁用", LIKES_NS);
        s_open = false;
        return;
    }
    s_open = true;

    /* 10 个桶全部载入 RAM 缓存 (之后查询走内存, 浏览器逐行判断不碰 NVS) */
    uint32_t *tmp = likes_alloc(MAX_BUCKET_HASHES * sizeof(uint32_t));
    if (tmp) {
        for (int b = 0; b < LIKES_BUCKETS; b++) {
            int n = load_bucket(b, tmp, MAX_BUCKET_HASHES);
            for (int i = 0; i < n; i++) cache_push(tmp[i]);
        }
        heap_caps_free(tmp);
    }
    ESP_LOGI(TAG, "已载入 %d 个喜欢", s_cache_n);
}

bool likes_contains(uint32_t h)
{
    if (!s_open) return false;
    return cache_contains(h);
}

bool likes_add(uint32_t h)
{
    if (!s_open) return false;
    if (cache_contains(h)) return false;      /* 已存在 */
    if (!cache_push(h)) return false;
    save_bucket_from_cache((int)(h % LIKES_BUCKETS));
    return true;
}

bool likes_remove(uint32_t h)
{
    if (!s_open) return false;

    int idx = -1;
    for (int i = 0; i < s_cache_n; i++) {
        if (s_cache[i] == h) { idx = i; break; }
    }
    if (idx < 0) return false;

    cache_remove_at(idx);
    save_bucket_from_cache((int)(h % LIKES_BUCKETS));
    return true;
}
