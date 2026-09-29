#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "diskio.h"
#include "ff.h"
#include <stdio.h>
#include "esp_heap_caps.h"
#include "atomic_utils.h"
#include "power_mgr.h"
#include "played_bits.h"
#include "audio_task.h"
#include "sys_monitor.h"

/* 安全拷贝: 把 src 复制到 dst (目标大小 dst_sz), 保证结尾 '\0' */
static inline void buf_copy(char *dst, size_t dst_sz, const char *src)
{
    if (dst_sz == 0) {
        return;
    }
    size_t n = strnlen(src, dst_sz - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

#define TAG_SDMMC   "SDMMC"
#define TAG_DETECT  "SD_DETECT"

#define MOUNT_POINT  "/sdcard"       /* SD 卡挂载点 */
#define PIN_SD_DETECT 13             /* SD 卡插入检测引脚 (高=未插入) */

#define PIN_CLK 14                   /* SDMMC 时钟脚 */
#define PIN_CMD 15                   /* SDMMC 命令脚 */
#define PIN_D0   2                   /* SDMMC 数据线 (1bit 模式只需 D0) */

#define SAMPLE_COUNT  10             /* 采样次数 (取平均滤波) */
#define SAMPLE_DELAY_MS 10           /* 相邻两次采样间隔 */
#define SENSOR_INTERVAL_TICKS 20     /* 传感器采样周期计数 (每轮 50ms, 标称 1s; 采样本身另耗 ~200ms) */

#define CPU_TEMP_OFFSET_C  0.0f     /* CPU 内部温度传感器校准偏移 */
#define VBAT_CRITICAL_LOW_V  3.25f   /* 运行中临界电压 (低于此值直接深睡关机) */

/* 跨模块共享状态 */
volatile bool  g_sd_ready = false;      /* SD 卡就绪标志 */
volatile float g_vbat     = 0.0f;       /* 电池电压 (V) */
volatile float g_cpu_temp = 0.0f;       /* CPU 温度 (C) */
volatile int8_t g_sd_manual_rescan = 0;    /* 手动重扫目标 (SD_RESCAN_*) */
fs_cache_t     *g_fs_cache     = NULL;      /* 音乐文件缓存指针 (PSRAM) */
fs_cache_t     *g_novel_cache  = NULL;      /* 小说文件缓存指针 (PSRAM) */

/* 删除文件请求 (UI→本任务): 标志 + 参数, 结果写进 g_sd_delete_status */
static volatile bool   s_del_req  = false;
static sd_delete_req_t s_del_data;
volatile int           g_sd_delete_status = 0;   /* 0=空闲/进行中, 1=成功, -1=失败 */

/* 投递删除请求 (UI→本任务, 非阻塞): 写参数后置标志; 已有请求未处理或参数非法返回 false */
bool sd_request_delete_file(const char *group, const char *name, int idx)
{
    if (s_del_req || !group || !name || idx < 0) return false;

    g_sd_delete_status = 0;                                   /* 先清状态 */
    buf_copy(s_del_data.group, sizeof(s_del_data.group), group);   /* 再写参数 */
    buf_copy(s_del_data.name,  sizeof(s_del_data.name),  name);
    s_del_data.idx = idx;
    s_del_req = true;                                         /* 后置标志 (release) */
    return true;
}

static sdmmc_card_t *s_card    = NULL;   /* SD 卡信息结构体 */
static bool          s_mounted = false;  /* 是否已挂载 */

static fs_cache_t   *s_fs_cache_owned = NULL;   /* 本模块持有的音乐缓存指针 (用于释放) */
static fs_cache_t   *s_fs_cache_retire = NULL;  /* 已下线待回收的旧音乐缓存 (交由 UI 任务释放) */
static fs_cache_t   *s_novel_cache_owned = NULL;  /* 本模块持有的小说缓存指针 */
static fs_cache_t   *s_novel_cache_retire = NULL; /* 已下线待回收的旧小说缓存 */
static SemaphoreHandle_t s_sd_fs_mutex = NULL;  /* SD/FATFS 访问互斥 (卸载与解码器 IO 互斥) */

static sd_event_cb_t  s_event_cb  = NULL;   /* 磁盘事件回调 */
static void          *s_event_ctx = NULL;   /* 回调用户数据 */

/* 由 group/name 拼出真实路径 (group 用真实 '/' 分隔):
 * group="sdcard"        → /sdcard/name
 * group="sdcard/a/b"    → /sdcard/a/b/name */
void fs_build_real_path(const char *group, const char *name,
                        char *out, size_t out_size)
{
    const char *rel = group;
    if (strncmp(rel, "sdcard", 6) == 0) {   /* 去掉 "sdcard" 前缀 */
        rel += 6;
        if (*rel == '/') rel++;             /* 去掉分组分隔符 '/' */
    }
    if (*rel == '\0') {
        snprintf(out, out_size, "/sdcard/%s", name);   /* 根目录文件 */
        return;
    }
    snprintf(out, out_size, "/sdcard/%s/%s", rel, name);   /* 子目录文件 */
}

/* 从指定 .bin 读取文件索引到 PSRAM.
 * 该文件是 fs_cache_t 的序列化镜像: entries 的 name/group 字段在磁盘上存
 * "字符串池内偏移"; 一次读入后修正为绝对指针即可直接用, 无需逐文件解析.
 * path=索引文件路径; 返回 fs_cache_t 或 NULL. */
static fs_cache_t *sd_load_cache_bin(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        ESP_LOGW(TAG_SDMMC, "无法打开索引文件: %s", path);
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)sizeof(fs_cache_t)) {
        ESP_LOGW(TAG_SDMMC, "索引文件无效");
        close(fd);
        return NULL;
    }
    size_t size = (size_t)st.st_size;

    fs_cache_t *cache = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!cache) {
        ESP_LOGE(TAG_SDMMC, "PSRAM 分配失败: %zu 字节", size);
        close(fd);
        return NULL;
    }

    /* 一次顺序读入整块 */
    size_t got = 0;
    while (got < size) {
        ssize_t r = read(fd, (char *)cache + got, size - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);

    if (got != size || cache->magic != FS_CACHE_MAGIC ||
        cache->count < 0 || cache->pool_size > size) {
        ESP_LOGW(TAG_SDMMC, "索引校验失败 (magic=%08x count=%d)",
                 (unsigned)cache->magic, cache->count);
        heap_caps_free(cache);
        return NULL;
    }

    size_t expect = sizeof(fs_cache_t) + (size_t)cache->count * sizeof(fs_entry_t)
                    + cache->pool_size;
    if (expect != size) {
        ESP_LOGW(TAG_SDMMC, "索引大小不符 (expect=%zu size=%zu)", expect, size);
        heap_caps_free(cache);
        return NULL;
    }

    /* 池内偏移 → 绝对指针 (单遍修正) */
    char *pool = (char *)(cache->entries + cache->count);
    for (int i = 0; i < cache->count; i++) {
        fs_entry_t *e = &cache->entries[i];
        uintptr_t noff = (uintptr_t)e->name;
        uintptr_t goff = (uintptr_t)e->group;
        if (noff >= cache->pool_size || goff >= cache->pool_size) {
            ESP_LOGW(TAG_SDMMC, "索引偏移越界 (i=%d)", i);
            heap_caps_free(cache);
            return NULL;
        }
        e->name  = pool + noff;
        e->group = pool + goff;
    }

    ESP_LOGI(TAG_SDMMC, "PSRAM 缓存已加载: %d 条目, %zu 字节 (池 %zu)",
             cache->count, size, cache->pool_size);
    return cache;
}

/* 扫描 SD 卡根目录: 统计普通文件数 (仅计数, 未输出) 并打印容量信息 (诊断用) */
static void sd_scan_files(void)
{
    int count = 0;

    DIR *dir = opendir(MOUNT_POINT);
    if (!dir) {
        ESP_LOGE(TAG_SDMMC, "无法打开目录");
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) {
            count++;   /* 数根目录普通文件数 */
        }
    }
    closedir(dir);

    /* 容量信息 (FatFs 查询) */
    FATFS *fs;
    DWORD free_clst;
    FRESULT fr = f_getfree(MOUNT_POINT, &free_clst, &fs);
    if (fr == FR_OK) {
        uint64_t total_kb = ((uint64_t)fs->n_fatent - 2) * fs->csize / 2;   /* 总量 */
        uint64_t free_kb  = (uint64_t)free_clst * fs->csize / 2;            /* 剩余 */
        ESP_LOGI(TAG_SDMMC, "存储: 总量 %llu KB, 剩余 %llu KB",
                 total_kb, free_kb);
    }
}

/* 在缓存中按完整路径查找文件, 输出其 group/name (供 UI 定位分组) */
bool fs_cache_find_by_path(const char *path,
                           char *group_out, size_t group_size,
                           char *name_out, size_t name_size)
{
    if (!path || !path[0] || !g_fs_cache) return false;

    for (int i = 0; i < g_fs_cache->count; i++) {
        fs_entry_t *e = &g_fs_cache->entries[i];
        if (e->is_dir) continue;   /* 跳过目录条目 */

        char real[512];
        fs_build_real_path(e->group, e->name, real, sizeof(real));   /* 拼真实路径 */
        if (strcmp(real, path) == 0) {
            if (group_out && group_size > 0) {   /* 输出 group */
                buf_copy(group_out, group_size, e->group);
            }
            if (name_out && name_size > 0) {     /* 输出 name */
                buf_copy(name_out, name_size, e->name);
            }
            return true;
        }
    }
    return false;
}

/* 探测并挂载 SD 卡: 成功则扫描音乐 + 载入 index.bin 缓存, 最后置 g_sd_ready */
void sdmmc_disk_init(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,   /* 挂载失败不格式化 (保护数据) */
        .max_files = 5,                    /* 同时打开文件数上限 */
        .allocation_unit_size = 16 * 1024, /* 分配单元 16KB */
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;                                /* 1bit 模式 */
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP; /* 使用内部上拉 */

#ifdef CONFIG_SOC_SDMMC_USE_GPIO_MATRIX   /* 某些芯片 SDMMC 引脚可映射, 指定自定义引脚 */
    slot_config.clk = PIN_CLK;
    slot_config.cmd = PIN_CMD;
    slot_config.d0  = PIN_D0;
#endif

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot_config,
                                            &mount_config, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_SDMMC, "挂载失败 (%s)", esp_err_to_name(ret));
        atomic_store_bool(&g_sd_ready, false);
        if (s_event_cb) s_event_cb("mount_failed", s_event_ctx);
        return;
    }

    sdmmc_card_print_info(stdout, s_card);   /* 打印卡信息 (容量/类型等) */
    s_mounted = true;
    ESP_LOGI(TAG_SDMMC, "SD 卡已挂载");

    /* 确保根目录下 "音乐" / "小说" 两个目录存在 (不存在则创建) */
    if (mkdir("/sdcard/音乐", 0777) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG_SDMMC, "创建 /sdcard/音乐 失败: %s", strerror(errno));
    }
    if (mkdir("/sdcard/小说", 0777) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG_SDMMC, "创建 /sdcard/小说 失败: %s", strerror(errno));
    }

    sd_scan_files();           /* 诊断: 根目录文件数/容量 */
    music_scan_init();         /* 扫描/加载音乐缓存 (/sdcard/音乐) */
    novel_scan_init();         /* 扫描/加载小说缓存 (/sdcard/小说) */

    s_fs_cache_owned    = sd_load_cache_bin(FS_CACHE_BIN_PATH);      /* 音乐 index.bin */
    s_novel_cache_owned = sd_load_cache_bin(NOVEL_CACHE_BIN_PATH);   /* 小说 novel.bin */
    g_fs_cache    = s_fs_cache_owned;
    g_novel_cache = s_novel_cache_owned;
    atomic_store_bool(&g_sd_ready, true);

    if (s_event_cb) s_event_cb("mounted", s_event_ctx);
}

/* 卸载 SD 卡: 先置 g_sd_ready=false 让 audio 释放文件, 排空 FATFS 后在锁内卸载 */
void sdmmc_disk_deinit(void)
{
    if (!s_mounted) return;

    atomic_store_bool(&g_sd_ready, false);   /* 先通知 audio 停止占用 SD */

    /* 先置空缓存指针; 实际内存移交 retire, 由 UI 任务在确认无读者时释放,
     * 避免本任务释放时 UI 已取得旧指针正在遍历导致 use-after-free */
    g_fs_cache    = NULL;
    g_novel_cache = NULL;
    if (s_fs_cache_owned) {
        s_fs_cache_retire = s_fs_cache_owned;
        s_fs_cache_owned = NULL;
    }
    if (s_novel_cache_owned) {
        s_novel_cache_retire = s_novel_cache_owned;
        s_novel_cache_owned = NULL;
    }

    /* 等 audio 关闭解码器并释放打开的文件, 同时排空在途 FATFS 调用:
     * 反复尝试取锁; 拿到锁且确认无打开文件后再卸载. 这样卸载时卷锁必为空,
     * 不会触发 _lock_close 断言 (audio 一见 g_sd_ready=false 即会关闭, 不依赖 UI) */
    bool locked = false;
    for (int i = 0; i < 300; i++) {          /* 最多 ~3s */
        sd_fs_lock();
        if (!atomic_load_bool(&g_audio_decoder_open)) { locked = true; break; }
        sd_fs_unlock();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!locked) sd_fs_lock();               /* 兜底: 仍阻塞取锁, 保证卸载时无在途调用 */

    esp_err_t ret = esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);
    sd_fs_unlock();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_SDMMC, "卸载失败 (%s)", esp_err_to_name(ret));
    }

    s_card    = NULL;
    s_mounted = false;
    ESP_LOGI(TAG_SDMMC, "SD 卡已卸载");

    if (s_event_cb) s_event_cb("unmounted", s_event_ctx);
}

/* 获取/释放 SD/FATFS 互斥锁 (跨任务文件操作与卸载互斥) */
void sd_fs_lock(void)
{
    if (s_sd_fs_mutex) xSemaphoreTake(s_sd_fs_mutex, portMAX_DELAY);
}

/* 释放 SD/FATFS 互斥锁 */
void sd_fs_unlock(void)
{
    if (s_sd_fs_mutex) xSemaphoreGive(s_sd_fs_mutex);
}

/* 回收已下线的旧缓存. 必须由 UI 任务调用: UI 是 g_fs_cache 的唯一遍历者,
 * 在 UI 上下文中释放可保证没有读者正在使用旧指针 (见 fs_cache_reap 声明) */
void fs_cache_reap(void)
{
    if (s_fs_cache_retire) {
        heap_caps_free(s_fs_cache_retire);
        s_fs_cache_retire = NULL;
    }
    if (s_novel_cache_retire) {
        heap_caps_free(s_novel_cache_retire);
        s_novel_cache_retire = NULL;
    }
}

/* 查询 SD 卡当前是否已挂载 */
bool sdmmc_disk_is_mounted(void)
{
    return s_mounted;
}

/* 注册磁盘事件回调: cb=回调函数, user_data=回调时透传的用户数据 */
void sdmmc_disk_set_event_callback(sd_event_cb_t cb, void *user_data)
{
    s_event_cb  = cb;
    s_event_ctx = user_data;
}

extern uint8_t temprature_sens_read(void);

/* 读取 CPU 内部温度 (华氏原始值 → 摄氏, 再减校准偏移) */
static float read_cpu_temp(void)
{
    return ((float)temprature_sens_read() - 32.0f) / 1.8f - CPU_TEMP_OFFSET_C;
}

/* 传感器采样: 多次取平均, 抑制 ADC 噪声.
 * 电池电压 ADC 由 power_mgr 持有, 这里复用其读取接口 (确保周期采样仍更新 g_vbat) */
static void sample_sensors(void)
{
    float temp_sum = 0.0f;
    float vbat_sum = 0.0f;
    int   vbat_valid = 0;

    for (int i = 0; i < SAMPLE_COUNT; i++) {
        temp_sum += read_cpu_temp();
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_DELAY_MS));

        float v = power_mgr_vbat_read_once();
        if (v >= 0.0f) { vbat_sum += v; vbat_valid++; }   /* 只累加有效采样 */
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_DELAY_MS));
    }

    float temp_avg = temp_sum / (float)SAMPLE_COUNT;   /* 温度平均 */
    /* 无有效电压采样则发布负值 (下游据此跳过低压判断) */
    float vbat_avg = vbat_valid ? (vbat_sum / (float)vbat_valid) : -1.0f;

    atomic_store_float(&g_cpu_temp, temp_avg);   /* 原子发布, 供 UI/控制台读取 */
    atomic_store_float(&g_vbat, vbat_avg);
}

/* 系统监视主循环: SD 插拔处理 / 延迟删除 / 手动重扫 / 周期传感器采样与低压关机 */
static void sys_monitor_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(150));
    gpio_set_direction(PIN_SD_DETECT, GPIO_MODE_INPUT);   /* SD 检测脚设为输入 */

    bool last = (gpio_get_level(PIN_SD_DETECT) == 1);   /* 初始检测电平 (true=未插入) */
    int  tick = SENSOR_INTERVAL_TICKS;                  /* 采样倒计时, 初始即采样 */

    ESP_LOGI(TAG_DETECT, "启动, GPIO%d 初始=%s", PIN_SD_DETECT, last ? "未插入" : "已插入");

    if (!last) {   /* 开机时卡已在 → 直接挂载 */
        sdmmc_disk_init();
    }

    while (1) {
        bool current = (gpio_get_level(PIN_SD_DETECT) == 1);   /* 当前电平 */

        /* 删除文件请求 (UI 已冻结界面并停音频): 删除并把结果回报给 UI */
        if (s_del_req) {
            char path[512];
            fs_build_real_path(s_del_data.group, s_del_data.name, path, sizeof(path));
            ESP_LOGI(TAG_DETECT, "执行删除: %s", path);
            int r = remove(path);
            if (r != 0) ESP_LOGW(TAG_DETECT, "删除失败: %s", path);
            g_sd_delete_status = (r == 0) ? 1 : -1;   /* 1=成功, -1=失败 */
            s_del_req = false;
        }

        /* 手动重扫: 模拟拔卡→插卡流程, 只对目标根置强制全量扫描标志 */
        if (g_sd_manual_rescan != 0) {
            int8_t what = g_sd_manual_rescan;
            g_sd_manual_rescan = 0;
            ESP_LOGI(TAG_DETECT, "手动触发重新扫描 (target=%d)", (int)what);
            if (what == SD_RESCAN_ALL || what == SD_RESCAN_MUSIC) g_music_scan_force = true;
            if (what == SD_RESCAN_ALL || what == SD_RESCAN_NOVEL) g_novel_scan_force = true;
            sdmmc_disk_deinit();
            vTaskDelay(pdMS_TO_TICKS(500));
            sdmmc_disk_init();
            last = (gpio_get_level(PIN_SD_DETECT) == 1);
        }

        if (last && !current) {   /* 高→低: 插入 */
            ESP_LOGI(TAG_DETECT, "检测到 SD 卡插入");
            vTaskDelay(pdMS_TO_TICKS(500));   /* 等卡稳定 */
            sdmmc_disk_init();
        }

        if (!last && current) {   /* 低→高: 拔出 */
            ESP_LOGI(TAG_DETECT, "检测到 SD 卡拔出");
            sdmmc_disk_deinit();
        }

        last = current;

        if (++tick >= SENSOR_INTERVAL_TICKS) {   /* 每 SENSOR_INTERVAL_TICKS 次循环采一次传感器 */
            tick = 0;
            sample_sensors();

            /* 周期低压检测: 低于临界值 → 无动画直接深睡 (深度睡眠即重启系统, 无需善后).
             * g_vbat<0 表示 ADC 不可用, 必须排除, 否则会被误判为低压 */
            if (g_vbat > 0.0f && g_vbat < VBAT_CRITICAL_LOW_V) {
                power_mgr_critical_shutdown();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));   /* 轮询周期 50ms (提高拔卡检测响应) */
    }
}

/* 启动系统监视任务 (固定 core 1) */
void sys_monitor_init(void)
{
    if (!s_sd_fs_mutex) s_sd_fs_mutex = xSemaphoreCreateMutex();   /* SD/FATFS 互斥 (须在任务及各 IO 之前建好) */
    xTaskCreatePinnedToCore(sys_monitor_task, "sys_monitor", 8192, NULL, 1, NULL, 1);
}
