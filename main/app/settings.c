#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "atomic_utils.h"
#include "settings.h"

#define SETTINGS_NS  "player"   /* NVS 统一命名空间, 所有设置都存这里 */

static const char *TAG = "SETTINGS";

/* ──────────────────────────── 音量 ──────────────────────────── */
#define VOL_KEY    "volume"

static volatile int32_t s_volume = 64;          /* 当前音量 (0~127), 原子访问, 供多任务读取 */
static int32_t          s_vol_last_saved = -1;  /* 上次成功写盘的音量, 用于去重避免频繁擦写 NVS */

/* 读取当前音量 (原子操作, 返回 0~127) */
int32_t volume_get(void)
{
    return atomic_load_i32(&s_volume);
}

/* 设定音量: v=目标音量, 内部钳制到 [VOLUME_MIN, VOLUME_MAX] */
void volume_set(int32_t v)
{
    if (v < VOLUME_MIN) v = VOLUME_MIN;
    if (v > VOLUME_MAX) v = VOLUME_MAX;
    atomic_store_i32(&s_volume, v);
}

/* 音量增减: delta=增减量 (可为负, 如按一下耳机+键传 +8) */
void volume_inc(int32_t delta)
{
    volume_set(volume_get() + delta);
}

/* 开机时从 NVS 恢复音量 (只读命名空间, 失败则保持默认 64) */
void volume_load_from_nvs(void)
{
    nvs_handle_t handle;
    if (nvs_open(SETTINGS_NS, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }

    int32_t v = VOLUME_MIN;
    esp_err_t ret = nvs_get_i32(handle, VOL_KEY, &v);   /* 读取, 失败时 v 保持初值 */
    nvs_close(handle);

    /* 值合法才采纳, 防止 NVS 里残留脏数据 */
    if (ret == ESP_OK && v >= VOLUME_MIN && v <= VOLUME_MAX) {
        atomic_store_i32(&s_volume, v);
        s_vol_last_saved = v;                            /* 标记已保存, 避免刚开机就重复写盘 */
        ESP_LOGI(TAG, "已从 NVS 恢复音量: %d", v);
    }
}

/* 把当前音量写回 NVS; 与上次保存值相同则跳过 (防磨损) */
void volume_save_to_nvs(void)
{
    int32_t v = volume_get();
    if (v == s_vol_last_saved) return;                   /* 值没变, 不写 flash */

    nvs_handle_t handle;
    if (nvs_open(SETTINGS_NS, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open 失败");
        return;
    }

    esp_err_t ret = nvs_set_i32(handle, VOL_KEY, v);     /* 暂存在缓存, 需 commit 才落盘 */
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);                        /* 提交到 flash */
    }
    nvs_close(handle);

    if (ret == ESP_OK) {
        s_vol_last_saved = v;
        ESP_LOGI(TAG, "已保存音量: %d", v);
    } else {
        ESP_LOGW(TAG, "保存失败 (%s)", esp_err_to_name(ret));
    }
}

/* ──────────────────────────── 亮度 ──────────────────────────── */
#define BRIGHT_KEY   "brightness"

static uint8_t s_brightness = 128;   /* 当前背光亮度 (1~255), 仅 UI 任务读写 */

uint8_t brightness_get(void)
{
    return s_brightness;
}

void brightness_set(uint8_t v)
{
    s_brightness = v;
}

/* 开机从 NVS 恢复亮度 */
void brightness_load_from_nvs(void)
{
    nvs_handle_t handle;
    if (nvs_open(SETTINGS_NS, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }

    uint8_t v = BRIGHTNESS_DEFAULT;
    esp_err_t ret = nvs_get_u8(handle, BRIGHT_KEY, &v);
    nvs_close(handle);

    /* uint8_t 天然 ≤ 255, 只检查下限即可 */
    if (ret == ESP_OK && (int)v >= BRIGHTNESS_MIN) {
        s_brightness = v;
        ESP_LOGI(TAG, "已从 NVS 恢复亮度: %u", v);
    }
}

/* 把当前亮度写回 NVS */
void brightness_save_to_nvs(void)
{
    nvs_handle_t handle;
    if (nvs_open(SETTINGS_NS, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open 失败");
        return;
    }

    esp_err_t ret = nvs_set_u8(handle, BRIGHT_KEY, s_brightness);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "已保存亮度: %u", s_brightness);
    } else {
        ESP_LOGW(TAG, "保存失败 (%s)", esp_err_to_name(ret));
    }
}

/* ──────────────────────── 上次播放歌曲 ──────────────────────── */
#define LAST_KEY   "song"
#define LAST_MAX   512   /* 曲目路径最大长度 */

static char s_last[LAST_MAX] = {0};   /* 内存缓存的最后播放路径, 用于去重 */

/* 保存上次播放曲目路径到 NVS; path=曲目路径 */
void last_song_save(const char *path)
{
    if (!path || !path[0]) return;

    /* 路径未变化时跳过写 flash, 降低 NVS 磨损 */
    if (strcmp(s_last, path) == 0) return;

    nvs_handle_t handle;
    if (nvs_open(SETTINGS_NS, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open 失败");
        return;
    }

    esp_err_t ret = nvs_set_str(handle, LAST_KEY, path);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);

    if (ret == ESP_OK) {
        strncpy(s_last, path, sizeof(s_last) - 1);   /* 同步内存缓存 */
        s_last[sizeof(s_last) - 1] = '\0';
        ESP_LOGI(TAG, "已保存播放路径: %s", path);
    } else {
        ESP_LOGW(TAG, "保存失败 (%s)", esp_err_to_name(ret));
    }
}

/* 读取上次播放曲目: buf=输出缓冲, size=缓冲大小; 成功返回 true */
bool last_song_load(char *buf, size_t size)
{
    if (!buf || size == 0) return false;

    nvs_handle_t handle;
    if (nvs_open(SETTINGS_NS, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }

    size_t len = size;
    esp_err_t ret = nvs_get_str(handle, LAST_KEY, buf, &len);   /* len 输入=缓冲大小, 输出=实际长度 */
    nvs_close(handle);

    if (ret != ESP_OK || len == 0 || buf[0] == '\0') {
        return false;
    }

    strncpy(s_last, buf, sizeof(s_last) - 1);   /* 同步内存缓存 */
    s_last[sizeof(s_last) - 1] = '\0';
    return true;
}

/* ──────────────────────────── 播放模式 ──────────────────────────── */
#define MODE_KEY   "play_mode"

/* 从 NVS 读播放模式: mode=输出; 成功返回 true, 无记录返回 false */
bool settings_mode_load(play_mode_t *mode)
{
    nvs_handle_t h;
    if (nvs_open(SETTINGS_NS, NVS_READONLY, &h) != ESP_OK) return false;

    int32_t m = PLAY_MODE_SEQUENTIAL;
    esp_err_t ret = nvs_get_i32(h, MODE_KEY, &m);
    nvs_close(h);

    /* 校验枚举范围, 防脏数据 */
    if (ret == ESP_OK && m >= PLAY_MODE_SEQUENTIAL && m <= PLAY_MODE_RANDOM) {
        *mode = (play_mode_t)m;
        return true;
    }
    return false;
}

/* 保存播放模式到 NVS */
void settings_mode_save(play_mode_t mode)
{
    nvs_handle_t h;
    if (nvs_open(SETTINGS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, MODE_KEY, (int32_t)mode);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ──────────────────────────── 循环次数 ──────────────────────────── */
#define LOOP_KEY   "loop_count"

/* 读循环次数 (随机/顺序每首重复次数): 无记录/越界返回 1 */
int settings_loop_count_load(void)
{
    nvs_handle_t h;
    if (nvs_open(SETTINGS_NS, NVS_READONLY, &h) != ESP_OK) return LOOP_COUNT_MIN;

    int32_t v = LOOP_COUNT_MIN;
    esp_err_t ret = nvs_get_i32(h, LOOP_KEY, &v);
    nvs_close(h);

    if (ret == ESP_OK && v >= LOOP_COUNT_MIN && v <= LOOP_COUNT_MAX) return (int)v;
    return LOOP_COUNT_MIN;
}

/* 保存循环次数到 NVS (钳制到 1~9) */
void settings_loop_count_save(int n)
{
    if (n < LOOP_COUNT_MIN) n = LOOP_COUNT_MIN;
    if (n > LOOP_COUNT_MAX) n = LOOP_COUNT_MAX;

    nvs_handle_t h;
    if (nvs_open(SETTINGS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, LOOP_KEY, (int32_t)n);
        nvs_commit(h);
        nvs_close(h);
    }
}
