#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/errno.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "esp_heap_caps.h"
#include "ff.h"
#include "sys_monitor.h"

#define TAG_MUSIC_SCAN   "MUSIC_SCAN"

#define MOUNT_POINT       "/sdcard"              /* SD 卡挂载点 */
#define MUSIC_CACHE_DIR   FS_CACHE_DIR           /* 缓存目录 (隐藏在 SD 卡上) */
#define SPACE_FILE        FS_CACHE_DIR "/space.dat"  /* 空间快照文件 */
#define SPACE_THRESHOLD_KB 10                   /* 空间变化超过此值才重扫 */

volatile bool g_music_scan_force = false;   /* 强制全量扫描标志 (外部置位, 用完自动清除) */

/* 识别为音乐文件的扩展名 (含大小写) */
static const char *MUSIC_EXTENSIONS[] = {
    ".mp3", ".flac", ".wav", ".aac",
    ".MP3", ".FLAC", ".WAV", ".AAC",
};
static const int NUM_EXTENSIONS = sizeof(MUSIC_EXTENSIONS) / sizeof(MUSIC_EXTENSIONS[0]);   /* 扩展名条目数 */

/* 目录工作栈 (变长 LIFO, 全部在 PSRAM):
 * 记录格式 = [路径字节]['\0'][uint16 长度], 只存实际路径长度.
 * 用显式栈替代递归, 且弹出即回收, 峰值只跟"待处理前沿"有关. */
typedef struct {
    char  *buf;    /* 字节池 (PSRAM) */
    size_t used;   /* 已用字节 */
    size_t cap;    /* 容量 */
} dir_stack_t;

/* 初始化工作栈 (PSRAM) */
static bool dir_stack_init(dir_stack_t *s)
{
    s->cap  = 4096;
    s->used = 0;
    s->buf  = heap_caps_malloc(s->cap, MALLOC_CAP_SPIRAM);
    if (!s->buf) {
        ESP_LOGE(TAG_MUSIC_SCAN, "工作栈分配失败");
        return false;
    }
    return true;
}

/* 确保剩余空间可容纳 extra 字节, 不足则倍增扩容 */
static bool dir_stack_reserve(dir_stack_t *s, size_t extra)
{
    if (s->used + extra <= s->cap) {
        return true;
    }
    size_t ncap = s->cap ? s->cap : 4096;
    while (ncap < s->used + extra) {
        ncap *= 2;
    }
    char *nb = heap_caps_realloc(s->buf, ncap, MALLOC_CAP_SPIRAM);
    if (!nb) {
        ESP_LOGE(TAG_MUSIC_SCAN, "工作栈扩容失败");
        return false;
    }
    s->buf = nb;
    s->cap = ncap;
    return true;
}

/* 入栈一条路径: 写入 [路径][\0][uint16 长度] */
static bool dir_stack_push(dir_stack_t *s, const char *path)
{
    size_t len = strlen(path);
    if (len > 0xFFFF) {
        ESP_LOGW(TAG_MUSIC_SCAN, "路径过长, 跳过: %s", path);
        return false;
    }
    size_t need = len + 1 + sizeof(uint16_t);
    if (!dir_stack_reserve(s, need)) {
        return false;
    }
    char *p = s->buf + s->used;
    memcpy(p, path, len);
    p[len] = '\0';
    uint16_t l16 = (uint16_t)len;
    memcpy(p + len + 1, &l16, sizeof(l16));
    s->used += need;
    return true;
}

/* 入栈 "a/b" (直接拼接, 省去中间缓冲) */
static bool dir_stack_push2(dir_stack_t *s, const char *a, const char *b)
{
    size_t la = strlen(a);
    size_t lb = strlen(b);
    size_t len = la + 1 + lb;
    if (len > 0xFFFF) {
        ESP_LOGW(TAG_MUSIC_SCAN, "路径过长, 跳过: %s/%s", a, b);
        return false;
    }
    size_t need = len + 1 + sizeof(uint16_t);
    if (!dir_stack_reserve(s, need)) {
        return false;
    }
    char *p = s->buf + s->used;
    memcpy(p, a, la);
    p[la] = '/';
    memcpy(p + la + 1, b, lb);
    p[len] = '\0';
    uint16_t l16 = (uint16_t)len;
    memcpy(p + len + 1, &l16, sizeof(l16));
    s->used += need;
    return true;
}

/* 取栈顶路径 (从记录末尾读长度定位); 空栈返回 NULL */
static const char *dir_stack_top(const dir_stack_t *s)
{
    if (s->used == 0) {
        return NULL;
    }
    uint16_t len;
    memcpy(&len, s->buf + s->used - sizeof(uint16_t), sizeof(len));
    return s->buf + s->used - (len + 1 + sizeof(uint16_t));
}

/* 弹出栈顶 (LIFO, 立即回收该记录空间) */
static void dir_stack_pop(dir_stack_t *s)
{
    if (s->used == 0) {
        return;
    }
    uint16_t len;
    memcpy(&len, s->buf + s->used - sizeof(uint16_t), sizeof(len));
    s->used -= (len + 1 + sizeof(uint16_t));
}

/* 释放工作栈 */
static void dir_stack_free(dir_stack_t *s)
{
    heap_caps_free(s->buf);
    s->buf  = NULL;
    s->used = 0;
    s->cap  = 0;
}

/* 索引构建器: 扫描期间在 PSRAM 累积条目与字符串池.
 * 条目里的 name/group 字段暂存"池内偏移"(与最终磁盘格式一致), 扫描结束一次性写出. */
typedef struct {
    fs_entry_t *dirs;   int n_dirs;   int cap_dirs;    /* 目录条目 */
    fs_entry_t *files;  int n_files;  int cap_files;   /* 文件条目 */
    char       *pool;   size_t pool_used; size_t pool_cap;  /* 字符串池 */
} cache_builder_t;

/* 扫描用 scratch 缓冲 (PSRAM): 按需增长, 只保留一份复用 */
typedef struct {
    dir_stack_t stack;          /* 目录工作栈 */
    char *cur_dir;    size_t cur_dir_cap;    /* 当前目录稳定副本 */
    char *full;       size_t full_cap;       /* 文件完整路径 */
    cache_builder_t b;          /* 索引构建器 */
} scan_ctx_t;

/* 确保 *p 至少有 need 字节容量 (PSRAM), 返回缓冲指针 (失败返回 NULL) */
static char *ensure_cap(char **p, size_t *cap, size_t need)
{
    if (*cap >= need) {
        return *p;
    }
    size_t ncap = *cap ? *cap : 256;
    while (ncap < need) {
        ncap *= 2;
    }
    char *np = heap_caps_realloc(*p, ncap, MALLOC_CAP_SPIRAM);
    if (!np) {
        ESP_LOGE(TAG_MUSIC_SCAN, "缓冲扩容失败");
        return NULL;
    }
    *p = np;
    *cap = ncap;
    return np;
}

/* 判断文件名是否为音乐文件 (按扩展名匹配) */
static bool is_music_file(const char *name)
{
    const char *dot = strrchr(name, '.');   /* 最后一个点作为扩展名起点 */
    if (!dot) return false;

    for (int i = 0; i < NUM_EXTENSIONS; i++) {
        if (strcmp(dot, MUSIC_EXTENSIONS[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* 把目录绝对路径映射为分组串 (用真实 '/'):
 * /sdcard        → "sdcard"
 * /sdcard/a/b    → "sdcard/a/b" */
static void build_group(const char *dir_path, char *out, size_t out_size)
{
    const char *rel = dir_path;
    size_t mount_len = strlen(MOUNT_POINT);

    if (strncmp(dir_path, MOUNT_POINT, mount_len) == 0) {
        rel = dir_path + mount_len;   /* 跳过挂载点前缀 */
        if (*rel == '/') rel++;
    }

    if (*rel == '\0') {
        snprintf(out, out_size, "sdcard");
    } else {
        snprintf(out, out_size, "sdcard/%s", rel);
    }
}

/* 追加字符串到池, 返回池内偏移 (失败返回 UINT32_MAX) */
static uint32_t pool_append(cache_builder_t *b, const char *s)
{
    size_t len = strlen(s) + 1;
    if (b->pool_used + len > b->pool_cap) {
        size_t ncap = b->pool_cap ? b->pool_cap : 4096;
        while (ncap < b->pool_used + len) {
            ncap *= 2;
        }
        char *np = heap_caps_realloc(b->pool, ncap, MALLOC_CAP_SPIRAM);
        if (!np) {
            ESP_LOGE(TAG_MUSIC_SCAN, "字符串池扩容失败");
            return UINT32_MAX;
        }
        b->pool = np;
        b->pool_cap = ncap;
    }
    uint32_t off = (uint32_t)b->pool_used;
    memcpy(b->pool + off, s, len);
    b->pool_used += len;
    return off;
}

/* 追加一个条目 (偏移暂存在指针字段, 与磁盘格式一致) */
static bool entry_append(fs_entry_t **arr, int *n, int *cap,
                         uint32_t name_off, uint32_t group_off, bool is_dir)
{
    if (*n == *cap) {
        int ncap = *cap ? *cap * 2 : 64;
        fs_entry_t *na = heap_caps_realloc(*arr, (size_t)ncap * sizeof(fs_entry_t),
                                           MALLOC_CAP_SPIRAM);
        if (!na) {
            ESP_LOGE(TAG_MUSIC_SCAN, "条目数组扩容失败");
            return false;
        }
        *arr = na;
        *cap = ncap;
    }
    fs_entry_t *e = &(*arr)[*n];
    e->name  = (const char *)(uintptr_t)name_off;
    e->group = (const char *)(uintptr_t)group_off;
    e->is_dir = is_dir;
    (*n)++;
    return true;
}

/* 获取 SD 卡已用空间 (KB), 用 FatFs 直接查 */
static uint64_t get_used_space_kb(void)
{
    FATFS *fs;
    DWORD free_clst;
    FRESULT fr = f_getfree(MOUNT_POINT, &free_clst, &fs);
    if (fr != FR_OK) {
        ESP_LOGE(TAG_MUSIC_SCAN, "获取空间信息失败 (%d)", fr);
        return 0;
    }

    /* n_fatent-2 = 有效簇数; csize/2 = 每簇扇区数→KB (扇区 512B) */
    uint64_t total_kb = ((uint64_t)fs->n_fatent - 2) * fs->csize / 2;
    uint64_t free_kb  = (uint64_t)free_clst * fs->csize / 2;
    return total_kb - free_kb;
}

#define SPACE_MAGIC  "MUSICACHE2\n"   /* 空间缓存文件魔数 (只存已用空间 KB) */

/* 读取上次扫描时记录的已用空间 (KB); 无/损坏返回 0 */
static uint64_t read_space_cache(void)
{
    int fd = open(SPACE_FILE, O_RDONLY);
    if (fd < 0) {
        return 0;
    }

    char buf[48] = {0};
    ssize_t len = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (len <= (ssize_t)strlen(SPACE_MAGIC)) return 0;          /* 内容过短 */
    if (strncmp(buf, SPACE_MAGIC, strlen(SPACE_MAGIC)) != 0) return 0;   /* 魔数不符 */
    return strtoull(buf + strlen(SPACE_MAGIC), NULL, 10);       /* 解析数字 */
}

/* 把本次扫描的已用空间写入缓存文件 */
static void write_space_cache(uint64_t used_kb)
{
    int fd = open(SPACE_FILE, O_RDWR | O_CREAT | O_TRUNC, 0);
    if (fd < 0) {
        ESP_LOGE(TAG_MUSIC_SCAN, "无法创建空间缓存文件: %s", strerror(errno));
        return;
    }

    char buf[48];
    int len = snprintf(buf, sizeof(buf), SPACE_MAGIC "%llu", used_kb);
    write(fd, buf, len);
    close(fd);
}

/* 清理 .music_cache 下旧缓存: 文件名以 "sdcard" 开头的 legacy 文件, 及旧索引 index.bin/.tmp */
static void clean_cache_files(void)
{
    DIR *dir = opendir(MUSIC_CACHE_DIR);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type != DT_REG) continue;                /* 只处理普通文件 */
        const char *n = entry->d_name;
        if (strncmp(n, "sdcard", 6) == 0 ||                  /* legacy 缓存文件 */
            strcmp(n, "index.bin") == 0 ||
            strcmp(n, "index.bin.tmp") == 0) {
            char full_path[512];
            snprintf(full_path, sizeof(full_path), "%s/%s", MUSIC_CACHE_DIR, n);
            remove(full_path);
        }
    }
    closedir(dir);
}

/* 把构建器序列化为 index.bin (先写临时文件再改名, 防掉电半写). 成功返回 true */
static bool write_index_bin(cache_builder_t *b)
{
    char tmp_path[128];
    snprintf(tmp_path, sizeof(tmp_path), "%s/index.bin.tmp", FS_CACHE_DIR);

    int fd = open(tmp_path, O_RDWR | O_CREAT | O_TRUNC, 0);
    if (fd < 0) {
        ESP_LOGE(TAG_MUSIC_SCAN, "无法创建索引文件: %s", tmp_path);
        return false;
    }

    fs_cache_t hdr;
    hdr.magic     = FS_CACHE_MAGIC;
    hdr.count     = b->n_dirs + b->n_files;
    hdr.pool_size = b->pool_used;

    bool ok = true;
    if (write(fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) ok = false;
    if (ok && b->n_dirs) {
        size_t sz = (size_t)b->n_dirs * sizeof(fs_entry_t);
        if (write(fd, b->dirs, sz) != (ssize_t)sz) ok = false;
    }
    if (ok && b->n_files) {
        size_t sz = (size_t)b->n_files * sizeof(fs_entry_t);
        if (write(fd, b->files, sz) != (ssize_t)sz) ok = false;
    }
    if (ok && b->pool_used) {
        if (write(fd, b->pool, b->pool_used) != (ssize_t)b->pool_used) ok = false;
    }
    close(fd);

    if (!ok) {
        ESP_LOGE(TAG_MUSIC_SCAN, "索引写入失败");
        remove(tmp_path);
        return false;
    }

    remove(FS_CACHE_BIN_PATH);   /* FatFs rename 目标已存在时可能失败 */
    if (rename(tmp_path, FS_CACHE_BIN_PATH) != 0) {
        ESP_LOGE(TAG_MUSIC_SCAN, "索引改名失败: %s", strerror(errno));
        remove(tmp_path);
        return false;
    }
    ESP_LOGI(TAG_MUSIC_SCAN, "索引已写入: %d 条目, 池 %zu 字节",
             hdr.count, hdr.pool_size);
    return true;
}

/* 校验 index.bin 是否存在且完整有效 (与 sd_load_cache_bin 的校验一致: 头部 + 精确长度) */
static bool index_bin_valid(void)
{
    int fd = open(FS_CACHE_BIN_PATH, O_RDONLY);
    if (fd < 0) return false;

    fs_cache_t hdr;
    ssize_t n = read(fd, &hdr, sizeof(hdr));
    if (n != (ssize_t)sizeof(hdr) || hdr.magic != FS_CACHE_MAGIC || hdr.count < 0) {
        close(fd);
        return false;
    }

    struct stat st;
    bool ok = false;
    if (fstat(fd, &st) == 0) {
        size_t expect = sizeof(fs_cache_t) + (size_t)hdr.count * sizeof(fs_entry_t)
                        + hdr.pool_size;
        ok = ((size_t)st.st_size == expect);   /* 长度必须精确匹配, 防截断/多余被误判有效 */
    }
    close(fd);
    return ok;
}

/* 扫描单个目录: 目录/文件条目写入构建器, 子目录入栈 (不递归).
 * dir_path 为调用方提供的稳定副本, 避免入栈 realloc 使其失效. */
static void scan_one_dir(scan_ctx_t *ctx, const char *dir_path)
{
    DIR *dir = opendir(dir_path);
    if (!dir) {
        ESP_LOGE(TAG_MUSIC_SCAN, "无法打开目录: %s", dir_path);
        return;
    }

    size_t dl = strlen(dir_path);

    /* 本目录分组串 (其下子目录与文件共用同一份) */
    char group[FS_GROUP_MAX];
    build_group(dir_path, group, sizeof(group));
    uint32_t group_off = pool_append(&ctx->b, group);

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        if (strcmp(entry->d_name, ".music_cache") == 0) {
            continue;   /* 跳过缓存目录自身 */
        }

        if (strcmp(entry->d_name, "System Volume Information") == 0) {
            continue;   /* 跳过 Windows 系统目录 */
        }

        if (entry->d_type == DT_REG) {
            if (is_music_file(entry->d_name)) {
                /* 跳过 0 字节的损坏/残留文件 */
                char *full = ensure_cap(&ctx->full, &ctx->full_cap,
                                        dl + 1 + strlen(entry->d_name) + 1);
                if (!full) {
                    continue;
                }
                snprintf(full, ctx->full_cap, "%s/%s", dir_path, entry->d_name);
                struct stat st;
                if (stat(full, &st) == 0 && st.st_size > 0) {
                    uint32_t noff = pool_append(&ctx->b, entry->d_name);
                    entry_append(&ctx->b.files, &ctx->b.n_files, &ctx->b.cap_files,
                                 noff, group_off, false);
                } else {
                    ESP_LOGW(TAG_MUSIC_SCAN, "跳过空文件: %s", full);
                }
            }
        } else if (entry->d_type == DT_DIR) {
            /* 目录条目: name=本级目录名, group=父目录 (本目录) */
            uint32_t noff = pool_append(&ctx->b, entry->d_name);
            entry_append(&ctx->b.dirs, &ctx->b.n_dirs, &ctx->b.cap_dirs,
                         noff, group_off, true);
            dir_stack_push2(&ctx->stack, dir_path, entry->d_name);   /* 子目录入栈待处理 */
        }
    }

    closedir(dir);
}

/* 执行一次全量扫描: 清缓存 → 迭代扫目录 → 写索引 + 空间快照 */
static void music_scan_run(void)
{
    ESP_LOGI(TAG_MUSIC_SCAN, "开始扫描音乐文件...");

    int64_t t0 = esp_timer_get_time();

    clean_cache_files();

    scan_ctx_t ctx = {0};
    if (!dir_stack_init(&ctx.stack)) {
        return;
    }
    /* 字符串池 (PSRAM) */
    ctx.b.pool_cap = 4096;
    ctx.b.pool = heap_caps_malloc(ctx.b.pool_cap, MALLOC_CAP_SPIRAM);
    if (!ctx.b.pool) {
        ESP_LOGE(TAG_MUSIC_SCAN, "字符串池分配失败");
        dir_stack_free(&ctx.stack);
        return;
    }

    dir_stack_push(&ctx.stack, MOUNT_POINT);

    bool scan_ok = true;
    while (ctx.stack.used > 0) {
        const char *top = dir_stack_top(&ctx.stack);   /* 栈顶路径 */
        size_t tl = strlen(top);
        char *cur = ensure_cap(&ctx.cur_dir, &ctx.cur_dir_cap, tl + 1);
        if (!cur) {
            scan_ok = false;   /* 分配失败: 扫描不完整, 不得写出部分索引 */
            break;
        }
        memcpy(cur, top, tl + 1);   /* 先拷出稳定副本 (入栈会 realloc 栈池) */
        dir_stack_pop(&ctx.stack);  /* 再弹出 */
        scan_one_dir(&ctx, cur);
    }

    bool wrote = false;
    if (scan_ok) {
        wrote = write_index_bin(&ctx.b);
    } else {
        ESP_LOGW(TAG_MUSIC_SCAN, "扫描未完成(内存不足), 跳过写索引, 下次开机重扫");
    }

    dir_stack_free(&ctx.stack);
    heap_caps_free(ctx.cur_dir);
    heap_caps_free(ctx.full);
    heap_caps_free(ctx.b.dirs);
    heap_caps_free(ctx.b.files);
    heap_caps_free(ctx.b.pool);

    uint64_t used_kb = get_used_space_kb();
    if (wrote) write_space_cache(used_kb);   /* 仅索引成功写入才更新空间快照 */

    int64_t elapsed = esp_timer_get_time() - t0;
    ESP_LOGI(TAG_MUSIC_SCAN, "扫描完成, 耗时 %.2f ms", elapsed / 1000.0f);
}

/* 音乐扫描初始化: 根据空间变化决定是否重扫 (除非被强制).
 * 策略: SD 卡已用空间与上次记录偏差 >10KB → 说明曲目有增删, 重扫; 否则用缓存, 秒开. */
void music_scan_init(void)
{
    ESP_LOGI(TAG_MUSIC_SCAN, "初始化音乐文件扫描器");

    if (mkdir(MUSIC_CACHE_DIR, 0777) != 0 && errno != EEXIST) {   /* 确保缓存目录存在 */
        ESP_LOGE(TAG_MUSIC_SCAN, "无法创建缓存目录 %s: %s", MUSIC_CACHE_DIR, strerror(errno));
        return;
    }

    uint64_t current_used = get_used_space_kb();   /* 当前已用空间 */
    uint64_t cached_used = read_space_cache();     /* 上次记录的已用空间 */

    ESP_LOGI(TAG_MUSIC_SCAN, "当前已用空间: %llu KB, 缓存记录: %llu KB",
             current_used, cached_used);

    bool force = g_music_scan_force;   /* 外部强制标志 (一次性) */
    g_music_scan_force = false;
    if (force) {
        ESP_LOGI(TAG_MUSIC_SCAN, "手动触发重新扫描");
        music_scan_run();
        return;
    }

    if (cached_used == 0) {   /* 首次运行, 无缓存记录 */
        ESP_LOGI(TAG_MUSIC_SCAN, "无缓存记录，开始全量扫描");
        music_scan_run();
        return;
    }

    /* 比较当前与记录的空间, 超过阈值才重扫; 索引文件缺失/损坏也重扫 */
    int64_t diff = (int64_t)current_used - (int64_t)cached_used;
    if (diff < 0) diff = -diff;
    if (diff > SPACE_THRESHOLD_KB) {
        ESP_LOGI(TAG_MUSIC_SCAN, "空间变化 %lld KB 超过阈值 %d KB，重新扫描",
                 diff, SPACE_THRESHOLD_KB);
        music_scan_run();
    } else if (!index_bin_valid()) {
        ESP_LOGI(TAG_MUSIC_SCAN, "索引缺失或无效，重新扫描");
        music_scan_run();
    } else {
        ESP_LOGI(TAG_MUSIC_SCAN, "空间变化在阈值内，使用缓存");
    }
}
