#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "sys_monitor.h"
#include "played_bits.h"

static const char *TAG = "PBITS";

/*
 * 每个文件夹一条记录, 存 NVS "pbits" 命名空间:
 *   key   = "b<id>"   (id 单调递增, 绝不复用)
 *   value = 字符串 "1 <valid_bits> <path_hex> <bits_hex>"
 *
 * 注意: 本 IDF (5.5.5) 的 nvs_entry_find 不会枚举 BLOB (isIterableItem 明确排除
 * BLOB/BLOB_IDX), 只枚举字符串/基本类型, 所以这里用字符串 + 十六进制编码位图,
 * 以便插卡时能遍历所有记录做全量校验与残留清理。
 */

#define PB_NS        "pbits"        /* 独立命名空间, 与设置 "player" 隔离 */
#define PB_KEY_NEXT  "nextid"       /* u32: 下一个可用 id (单调递增, 不复用) */
#define PB_VER       1              /* value 格式版本 */
#define PB_VAL_MAX   4096           /* 单条 value 上限 (NVS 字符串上限 4000) */

/* 每个文件夹一条记录 (常驻 PSRAM) */
typedef struct {
    char     *group;        /* 分组串 (如 "sdcard/子目录") */
    uint32_t  id;           /* NVS 键号, 0 = 尚未建档 (懒建档) */
    uint32_t  valid_bits;   /* 有效比特位数 (= 该文件夹文件数) */
    uint32_t  zero_count;   /* 剩余 0 位数量 */
    uint8_t  *bits;         /* ceil(valid_bits/8) 字节 */
    bool      needs_persist;/* 长度不符需写回全 0 新长度 */
} pb_entry_t;

static pb_entry_t  *s_entries  = NULL;
static int          s_n        = 0;
static uint32_t     s_next_id  = 1;
static nvs_handle_t s_nvs      = 0;
static bool         s_nvs_open = false;

/* PSRAM 优先分配, 失败回退内部 RAM */
static void *pb_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_malloc(n, MALLOC_CAP_8BIT);
    return p;
}

static char *pb_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = pb_alloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static pb_entry_t *pb_find(const char *group)
{
    if (!group) return NULL;
    for (int i = 0; i < s_n; i++) {
        if (strcmp(s_entries[i].group, group) == 0) return &s_entries[i];
    }
    return NULL;
}

/* 统计有效位数内为 0 的比特数 (末字节填充位不计) */
static uint32_t pb_count_zero(const uint8_t *bits, uint32_t n)
{
    uint32_t z = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (!(bits[i >> 3] & (1u << (i & 7)))) z++;
    }
    return z;
}

static void pb_key(char *out, size_t out_size, uint32_t id)
{
    snprintf(out, out_size, "b%u", (unsigned)id);
}

static int pb_hex_nibble(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* 十六进制编码: out 需 2*n 字节; 返回写入字符数 */
static size_t pb_hex_encode(const uint8_t *in, size_t n, char *out)
{
    static const char H[] = "0123456789abcdef";
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        out[o++] = H[in[i] >> 4];
        out[o++] = H[in[i] & 0x0F];
    }
    return o;
}

/* 十六进制解码: 返回字节数, -1 表示非法 */
static int pb_hex_decode(const char *in, size_t hexlen, uint8_t *out, size_t out_max)
{
    if (hexlen % 2) return -1;
    size_t n = hexlen / 2;
    if (n > out_max) return -1;
    for (size_t i = 0; i < n; i++) {
        int hi = pb_hex_nibble(in[2 * i]);
        int lo = pb_hex_nibble(in[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return (int)n;
}

/* 把一条位图编码成字符串并写入 NVS (含 commit); id==0 或句柄未开则跳过 */
static void pb_write_entry(pb_entry_t *pe)
{
    if (!s_nvs_open || !pe || pe->id == 0) return;

    size_t path_len = strlen(pe->group);
    size_t nbytes   = (pe->valid_bits + 7) / 8;
    /* "1 <bits> <pathhex> <bitshex>\0" */
    size_t need = 2 + 12 + 1 + path_len * 2 + 1 + nbytes * 2 + 1;
    if (need > PB_VAL_MAX) {
        ESP_LOGW(TAG, "文件夹 %s 位图过大 (%zu), 跳过持久化", pe->group, need);
        return;
    }

    char *buf = pb_alloc(need);
    if (!buf) return;

    size_t pos = (size_t)snprintf(buf, need, "%d %u ", PB_VER, (unsigned)pe->valid_bits);
    pos += pb_hex_encode((const uint8_t *)pe->group, path_len, buf + pos);
    buf[pos++] = ' ';
    pos += pb_hex_encode(pe->bits, nbytes, buf + pos);
    buf[pos] = '\0';

    char key[16];
    pb_key(key, sizeof(key), pe->id);
    esp_err_t r = nvs_set_str(s_nvs, key, buf);
    if (r == ESP_OK) r = nvs_commit(s_nvs);
    if (r != ESP_OK) ESP_LOGW(TAG, "写位图失败 %s: %s", key, esp_err_to_name(r));

    heap_caps_free(buf);
}

static void pb_free_entries(void)
{
    for (int i = 0; i < s_n; i++) {
        if (s_entries[i].group) heap_caps_free(s_entries[i].group);
        if (s_entries[i].bits)  heap_caps_free(s_entries[i].bits);
    }
    if (s_entries) heap_caps_free(s_entries);
    s_entries = NULL;
    s_n = 0;
}

/* 待删 id 列表 (遍历迭代器期间不能删, 先收集) */
typedef struct {
    uint32_t *ids;
    int       n;
    int       cap;
} pb_del_list_t;

static void pb_del_push(pb_del_list_t *dl, uint32_t id)
{
    if (dl->n == dl->cap) {
        int nc = dl->cap ? dl->cap * 2 : 8;
        uint32_t *nd = heap_caps_realloc(dl->ids, (size_t)nc * sizeof(uint32_t),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!nd) nd = heap_caps_realloc(dl->ids, (size_t)nc * sizeof(uint32_t), MALLOC_CAP_8BIT);
        if (!nd) return;
        dl->ids = nd;
        dl->cap = nc;
    }
    dl->ids[dl->n++] = id;
}

/* 解析一条 value, 按路径匹配当前文件夹: 采纳 / 待删 / 标记重置 */
static void pb_parse_value(const char *val, uint32_t id, uint32_t *max_id, pb_del_list_t *del)
{
    char *end = NULL;
    unsigned long ver = strtoul(val, &end, 10);
    if (ver != PB_VER || !end || *end != ' ') return;

    unsigned long vb = strtoul(end + 1, &end, 10);
    if (!end || *end != ' ') return;

    char *pathhex = end + 1;
    char *sp = strchr(pathhex, ' ');
    if (!sp) return;
    size_t pathhex_len = (size_t)(sp - pathhex);

    char path[FS_GROUP_MAX];
    int plen = pb_hex_decode(pathhex, pathhex_len, (uint8_t *)path, sizeof(path) - 1);
    if (plen <= 0) return;
    path[plen] = '\0';

    /* 兼容旧格式: 早期分组串用 '%' 表示 '/' */
    for (char *p = path; *p; p++) {
        if (*p == '%') *p = '/';
    }

    pb_entry_t *match = pb_find(path);
    if (!match) {                 /* 路径不在当前 SD: 待删 */
        pb_del_push(del, id);
        return;
    }

    match->id = id;
    if (id > *max_id) *max_id = id;

    if (match->valid_bits != (uint32_t)vb) {
        /* 长度不符: 保留 id, 迭代结束后写回全 0 新长度 */
        match->needs_persist = true;
        return;
    }

    const char *bitshex = sp + 1;
    size_t nbytes = ((uint32_t)vb + 7) / 8;
    if (nbytes && match->bits) {
        if (pb_hex_decode(bitshex, strlen(bitshex), match->bits, nbytes) >= 0) {
            match->zero_count = pb_count_zero(match->bits, (uint32_t)vb);
        }
    }
}

void played_bits_on_sd_ready(void)
{
    played_bits_on_sd_remove();   /* 先清干净, 支持重新扫描 */

    if (!g_fs_cache) return;

    /* 打开 pbits 命名空间: 整个挂载期间复用同一个 handle */
    if (nvs_open(PB_NS, NVS_READWRITE, &s_nvs) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open(%s) 失败, 随机去重禁用", PB_NS);
        s_nvs_open = false;
        return;
    }
    s_nvs_open = true;

    /* 1. 从缓存建立每个文件夹的条目 (只统计文件, 与 player_file_name_at 枚举一致) */
    int cap = 0;
    for (int i = 0; i < g_fs_cache->count; i++) {
        fs_entry_t *e = &g_fs_cache->entries[i];
        if (e->is_dir) continue;

        pb_entry_t *pe = pb_find(e->group);
        if (!pe) {
            if (s_n == cap) {
                int ncap = cap ? cap * 2 : 8;
                pb_entry_t *ne = heap_caps_realloc(s_entries, (size_t)ncap * sizeof(pb_entry_t),
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (!ne) ne = heap_caps_realloc(s_entries, (size_t)ncap * sizeof(pb_entry_t),
                                                MALLOC_CAP_8BIT);
                if (!ne) { ESP_LOGE(TAG, "条目分配失败"); break; }
                s_entries = ne;
                cap = ncap;
            }
            pe = &s_entries[s_n++];
            memset(pe, 0, sizeof(*pe));
            pe->group = pb_strdup(e->group);
            if (!pe->group) { s_n--; break; }
        }
        pe->valid_bits++;
    }

    /* 分配位图, 全 0; zero_count = 有效位数 */
    for (int i = 0; i < s_n; i++) {
        pb_entry_t *pe = &s_entries[i];
        size_t nbytes = (pe->valid_bits + 7) / 8;
        if (nbytes) {
            pe->bits = pb_alloc(nbytes);
            if (!pe->bits) { ESP_LOGE(TAG, "位图分配失败"); continue; }
            memset(pe->bits, 0, nbytes);
        }
        pe->zero_count = pe->valid_bits;
        pe->id = 0;
    }

    uint32_t stored_next = 0;
    if (nvs_get_u32(s_nvs, PB_KEY_NEXT, &stored_next) != ESP_OK) stored_next = 0;
    uint32_t max_id = 0;

    /* 2. 遍历 pbits 中所有字符串记录, 按 value 内路径匹配当前文件夹 */
    pb_del_list_t del = {0};

    nvs_iterator_t it  = NULL;
    esp_err_t res = nvs_entry_find_in_handle(s_nvs, NVS_TYPE_STR, &it);
    while (res == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);

        if (info.key[0] == 'b') {
            char *end = NULL;
            unsigned long id = strtoul(info.key + 1, &end, 10);
            if (end && *end == '\0' && id > 0 && id <= 0xFFFFFFFFul) {
                size_t len = 0;
                if (nvs_get_str(s_nvs, info.key, NULL, &len) == ESP_OK && len > 1 && len <= PB_VAL_MAX) {
                    char *val = pb_alloc(len);
                    if (val && nvs_get_str(s_nvs, info.key, val, &len) == ESP_OK) {
                        pb_parse_value(val, (uint32_t)id, &max_id, &del);
                    }
                    if (val) heap_caps_free(val);
                }
            }
        }

        res = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);

    /* 3. 迭代器已释放: 统一删除残留 + 写回长度不符的重置位图 */
    for (int i = 0; i < del.n; i++) {
        char key[16];
        pb_key(key, sizeof(key), del.ids[i]);
        nvs_erase_key(s_nvs, key);
    }
    if (del.ids) heap_caps_free(del.ids);

    for (int i = 0; i < s_n; i++) {
        if (s_entries[i].needs_persist) {
            pb_write_entry(&s_entries[i]);
            s_entries[i].needs_persist = false;
        }
    }

    /* 4. nextid 单调递增, 保证 id 绝不复用 */
    s_next_id = (stored_next > max_id + 1) ? stored_next : (max_id + 1);
    if (s_next_id != stored_next) nvs_set_u32(s_nvs, PB_KEY_NEXT, s_next_id);
    nvs_commit(s_nvs);

    ESP_LOGI(TAG, "已载入 %d 个文件夹位图 (next_id=%u)", s_n, (unsigned)s_next_id);
}

void played_bits_on_sd_remove(void)
{
    pb_free_entries();
    if (s_nvs_open) {
        nvs_close(s_nvs);
        s_nvs_open = false;
    }
    s_next_id = 1;
}

int played_bits_pick(const char *group)
{
    pb_entry_t *pe = pb_find(group);
    if (!pe || pe->valid_bits == 0 || !pe->bits) return -1;

    /* 懒清零: 全部播完 (全 1) 后, 下次取歌前清零 */
    if (pe->zero_count == 0) {
        size_t nbytes = (pe->valid_bits + 7) / 8;
        memset(pe->bits, 0, nbytes);
        pe->zero_count = pe->valid_bits;
        pb_write_entry(pe);
    }

    uint32_t k = esp_random() % pe->zero_count;
    for (uint32_t i = 0; i < pe->valid_bits; i++) {
        if (!(pe->bits[i >> 3] & (1u << (i & 7)))) {
            if (k == 0) return (int)i;
            k--;
        }
    }
    return -1;
}

bool played_bits_mark(const char *group, int idx)
{
    pb_entry_t *pe = pb_find(group);
    if (!pe || idx < 0 || (uint32_t)idx >= pe->valid_bits || !pe->bits) return false;

    uint32_t byte = (uint32_t)idx >> 3;
    uint8_t  mask = (uint8_t)(1u << ((uint32_t)idx & 7));
    if (pe->bits[byte] & mask) return true;   /* 已置位 */

    pe->bits[byte] |= mask;
    if (pe->zero_count) pe->zero_count--;

    /* 懒建档: 首次置位才分配 id 并持久化 nextid */
    if (pe->id == 0) {
        pe->id = s_next_id++;
        if (s_nvs_open) {
            nvs_set_u32(s_nvs, PB_KEY_NEXT, s_next_id);
            nvs_commit(s_nvs);
        }
    }
    pb_write_entry(pe);
    return true;
}
