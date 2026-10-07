#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "esp_log.h"
#include "nvs.h"
#include "atomic_utils.h"
#include "song_hash.h"
#include "settings.h"

#define SETTINGS_NS  "player"   /* NVS 统一命名空间, 所有设置都存这里 */

static const char *TAG = "SETTINGS";

/* ──────────────────────── 软件音量调节 ────────────────────────
 * 本优化主要面向低成本耳机/入门级音频设备。此类设备的音量调节级数通常约为 16 级，
 * 且多数未采用对数/感知音量映射，导致低音量区段步进偏大、响度变化不够均匀。
 *
 * 取舍：
 * 对于高品质耳机/音频设备，其通常具备更细的音量调节粒度、更优的音量控制策略
 * 以及更符合感知特性的映射曲线。因此，本优化可能使其音量映射偏离理论最优，
 * 引入轻微回退；但该差异在常规听音条件下通常没有明显差异。
 *
 * 本优化以牺牲少量高品质设备的音量映射最优性与调节细粒度，
 * 换取整体设备（尤其是低成本设备）音量调节体验的一致性、感知线性度与可用性。 */
/* ──────────────────────────── 音量 ────────────────────────────
 * 内部 32 档, 每档 {目标音量, 硬件音量, 软件增益%}.
 * 便宜耳机音量只按 8 步进(向下取整), 且低档跨得比高档大 —— 实测硬件幅度
 * 大致 ∝ 档值² (每 +8 的 dB 跨度随档号递减). 故用软件增益把每对上下两跳
 * 拉平: 配对上半档(目标 8k-4, 硬件 8k)的增益取该硬件跨度的一半,
 *   g_k = (k-1)/k        (k = 硬件档序号, 硬件 = 8k)
 * 例: 目标 12 → 硬件 16, 增益 1/2 = 50%; 目标 20 → 硬件 24, 增益 2/3 ≈ 66%.
 * 目标 4 的下锚点是静音无法取中点, 取经验值 50%; 末档硬件钳到 AVRCP 上限 127.
 * 增益只用整数百分比 (无 FPU). */
#define VOL_KEY       "volume"    /* 旧键: 直接存 0~127 目标音量 (仅用于迁移) */
#define VOL_IDX_KEY   "volidx"    /* 新键: 存档位 0~31 */

typedef struct {
    int16_t target;   /* 目标/显示音量 (4 的倍数) */
    int16_t hw;       /* 硬件音量 (8 的倍数, 末档钳到 127) */
    uint8_t gain;     /* 软件增益百分比 (0~100) */
} vol_step_t;

static const vol_step_t VOL_TABLE[VOLUME_STEPS] = {
    {  0,   0,   0}, {  4,   8,  50}, {  8,   8, 100}, { 12,  16,  50},
    { 16,  16, 100}, { 20,  24,  66}, { 24,  24, 100}, { 28,  32,  75},
    { 32,  32, 100}, { 36,  40,  80}, { 40,  40, 100}, { 44,  48,  83},
    { 48,  48, 100}, { 52,  56,  85}, { 56,  56, 100}, { 60,  64,  87},
    { 64,  64, 100}, { 68,  72,  88}, { 72,  72, 100}, { 76,  80,  90},
    { 80,  80, 100}, { 84,  88,  90}, { 88,  88, 100}, { 92,  96,  91},
    { 96,  96, 100}, {100, 104,  92}, {104, 104, 100}, {108, 112,  92},
    {112, 112, 100}, {116, 120,  93}, {120, 120, 100}, {124, 127,  93},
};

static volatile int32_t s_vol_index = 16;            /* 当前档位 0~31 (默认 16 → 音量 64), 原子访问 */
static int32_t          s_vol_idx_last_saved = -1;   /* 上次成功写盘的档位, 去重防磨损 */

/* 档位钳制到 [0, VOLUME_STEPS-1] */
static int32_t vol_clamp_index(int32_t idx)
{
    if (idx < 0) return 0;
    if (idx > VOLUME_STEPS - 1) return VOLUME_STEPS - 1;
    return idx;
}

int32_t volume_index_get(void)
{
    return atomic_load_i32(&s_vol_index);
}

void volume_set_index(int32_t idx)
{
    atomic_store_i32(&s_vol_index, vol_clamp_index(idx));
}

/* 档位增减: delta=档数 (可为负, 如按一下耳机+键传 +1) */
void volume_inc(int32_t delta)
{
    volume_set_index(volume_index_get() + delta);
}

/* 当前档位对应目标音量 (0/4/.../124), 供 UI 显示 */
int32_t volume_get(void)
{
    return VOL_TABLE[vol_clamp_index(volume_index_get())].target;
}

/* 当前档位对应硬件音量 (8 的倍数, 末档 127), 发蓝牙 */
int32_t volume_get_hw(void)
{
    return VOL_TABLE[vol_clamp_index(volume_index_get())].hw;
}

/* 当前档位对应软件增益 (%) 0~100 */
int32_t volume_get_gain(void)
{
    return VOL_TABLE[vol_clamp_index(volume_index_get())].gain;
}

/* 耳机回传的绝对音量 (0~127) → 最近档位. 耳机只按 8 步进, hw/4 落在偶数档 (增益 100%) */
void volume_set_from_hw(int32_t hw)
{
    if (hw < 0) hw = 0;
    if (hw > VOLUME_MAX) hw = VOLUME_MAX;
    volume_set_index(hw / 4);
}

/* 开机时从 NVS 恢复音量档位 (只读; 先新键, 再旧键迁移, 都失败保持默认 16) */
void volume_load_from_nvs(void)
{
    nvs_handle_t handle;
    if (nvs_open(SETTINGS_NS, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }

    int32_t idx = -1;
    esp_err_t ret = nvs_get_i32(handle, VOL_IDX_KEY, &idx);   /* 新键优先 */
    if (ret == ESP_OK && idx >= 0 && idx < VOLUME_STEPS) {
        nvs_close(handle);
        atomic_store_i32(&s_vol_index, idx);
        s_vol_idx_last_saved = idx;                        /* 标记已保存, 避免刚开机重复写盘 */
        ESP_LOGI(TAG, "已从 NVS 恢复音量档位: %d (音量 %d)", idx, VOL_TABLE[idx].target);
        return;
    }

    /* 迁移旧键 (0~127 目标音量): 换算为档位 */
    int32_t v = VOLUME_MIN;
    ret = nvs_get_i32(handle, VOL_KEY, &v);
    nvs_close(handle);
    if (ret == ESP_OK && v >= VOLUME_MIN && v <= VOLUME_MAX) {
        idx = vol_clamp_index(v / 4);
        atomic_store_i32(&s_vol_index, idx);
        s_vol_idx_last_saved = -1;                         /* 触发下次写成新键 */
        ESP_LOGI(TAG, "旧音量迁移: %d → 档位 %d (音量 %d)", v, idx, VOL_TABLE[idx].target);
    }
}

/* 把当前档位写回 NVS; 与上次保存值相同则跳过 (防磨损) */
void volume_save_to_nvs(void)
{
    int32_t idx = volume_index_get();
    if (idx == s_vol_idx_last_saved) return;               /* 值没变, 不写 flash */

    nvs_handle_t handle;
    if (nvs_open(SETTINGS_NS, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open 失败");
        return;
    }

    esp_err_t ret = nvs_set_i32(handle, VOL_IDX_KEY, idx);   /* 暂存在缓存, 需 commit 才落盘 */
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);                          /* 提交到 flash */
    }
    nvs_close(handle);

    if (ret == ESP_OK) {
        s_vol_idx_last_saved = idx;
        ESP_LOGI(TAG, "已保存音量档位: %d (音量 %d)", idx, VOL_TABLE[idx].target);
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

/* ──────────────────────── 上次播放歌曲 (含播放进度) ────────────────────────
 * 路径与进度存在同一个键里, 组成 blob: 路径(NUL 结尾) + uint16 千分比进度(0~1000).
 *
 * 为什么用千分比而不是"秒": 音频跳转接口 AUDIO_CMD_SEEK 的 param 只接受千分比,
 * 直接存千分比可在恢复时立即 seek, 不必等解码器异步解析出总时长再换算.
 *
 * 进度策略(见 ui_player.c): 仅对 >10 分钟的歌保存, 粒度为 1 分钟; 切歌置零. */
#define LAST_KEY       "song"
#define LAST_MAX       512   /* 曲目路径最大长度 */
#define LAST_PROG_LEN  2     /* 追加在路径后的进度字节数 (uint16 千分比) */

static char     s_last[LAST_MAX] = {0};   /* 内存缓存的最后播放路径, 用于去重 */
static uint16_t s_last_progress = 0;      /* 内存缓存的最后进度千分比, 用于去重防磨损 */

/* 把路径 + 进度打包写回 NVS; 成功后同步内存缓存.
 * 这是唯一的落盘点, last_song_save / last_song_progress_save 都经它写盘 */
static void last_song_write(const char *path, uint16_t progress)
{
    size_t plen = strlen(path);
    if (plen > LAST_MAX - 1) plen = LAST_MAX - 1;

    uint8_t blob[LAST_MAX + LAST_PROG_LEN];   /* 路径 + NUL + 进度 */
    memcpy(blob, path, plen);
    blob[plen] = '\0';
    memcpy(blob + plen + 1, &progress, LAST_PROG_LEN);   /* 原生小端 */
    size_t blen = plen + 1 + LAST_PROG_LEN;

    nvs_handle_t handle;
    if (nvs_open(SETTINGS_NS, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open 失败");
        return;
    }

    esp_err_t ret = nvs_set_blob(handle, LAST_KEY, blob, blen);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);

    if (ret == ESP_OK) {
        memcpy(s_last, blob, plen + 1);   /* 同步内存缓存 */
        s_last_progress = progress;
        ESP_LOGI(TAG, "已保存播放路径/进度: %s (千分比 %u)", path, progress);
    } else {
        ESP_LOGW(TAG, "保存失败 (%s)", esp_err_to_name(ret));
    }
}

/* 保存上次播放曲目路径到 NVS (进度置零); path=曲目路径.
 * 切歌或重播时调用, 即"切歌时自动置零" */
void last_song_save(const char *path)
{
    if (!path || !path[0]) return;

    /* 路径与进度都未变化时跳过写 flash, 降低 NVS 磨损 */
    if (strcmp(s_last, path) == 0 && s_last_progress == 0) return;

    last_song_write(path, 0);
}

/* 只更新进度 (路径沿用内存缓存), 写盘前与缓存比较去重.
 * progress=千分比 0~1000; 无当前曲目时 no-op */
void last_song_progress_save(uint16_t progress)
{
    if (!s_last[0]) return;
    if (progress == s_last_progress) return;   /* 未变化不写 flash */

    last_song_write(s_last, progress);
}

/* 读取上次播放曲目与进度: buf=路径输出缓冲, size=缓冲大小, progress=进度输出(可 NULL).
 * 成功返回 true. 兼容旧版仅字符串格式 (此时进度按 0 处理). */
bool last_song_load(char *buf, size_t size, uint16_t *progress)
{
    if (progress) *progress = 0;
    if (!buf || size == 0) return false;

    nvs_handle_t handle;
    if (nvs_open(SETTINGS_NS, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }

    /* NVS 的字符串与 blob 底层同为变长二进制, 旧字符串数据也能用 get_blob 读回:
     * 此时长度为 strlen+1, 不含进度字节, progress 自然保持 0. */
    uint8_t blob[LAST_MAX + LAST_PROG_LEN];
    size_t  blen = sizeof(blob);
    esp_err_t ret = nvs_get_blob(handle, LAST_KEY, blob, &blen);
    nvs_close(handle);

    if (ret != ESP_OK || blen == 0) {
        return false;
    }

    blob[sizeof(blob) - 1] = '\0';
    size_t plen = strnlen((const char *)blob, blen);
    if (plen == 0 || plen >= size) {   /* 空路径 / 输出缓冲太小 */
        return false;
    }

    memcpy(buf, blob, plen + 1);   /* 含 NUL */

    if (progress && blen >= plen + 1 + LAST_PROG_LEN) {
        memcpy(progress, blob + plen + 1, LAST_PROG_LEN);
    }

    strncpy(s_last, buf, sizeof(s_last) - 1);   /* 同步内存缓存 */
    s_last[sizeof(s_last) - 1] = '\0';
    s_last_progress = progress ? *progress : 0;
    return true;
}

/* ──────────────────────── 上次打开的小说 ──────────────────────── */
#define LAST_NOVEL_KEY   "novel"   /* NVS key: 上次打开的小说路径 */

static char s_last_novel[LAST_MAX] = {0};   /* 内存缓存, 用于去重 */

/* 保存上次打开的小说路径到 NVS; path=小说真实路径 */
void last_novel_save(const char *path)
{
    if (!path || !path[0]) return;
    if (strcmp(s_last_novel, path) == 0) return;   /* 未变化不写 flash */

    nvs_handle_t handle;
    if (nvs_open(SETTINGS_NS, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open 失败");
        return;
    }

    esp_err_t ret = nvs_set_str(handle, LAST_NOVEL_KEY, path);
    if (ret == ESP_OK) ret = nvs_commit(handle);
    nvs_close(handle);

    if (ret == ESP_OK) {
        strncpy(s_last_novel, path, sizeof(s_last_novel) - 1);
        s_last_novel[sizeof(s_last_novel) - 1] = '\0';
        ESP_LOGI(TAG, "已保存小说路径: %s", path);
    } else {
        ESP_LOGW(TAG, "保存失败 (%s)", esp_err_to_name(ret));
    }
}

/* 读取上次打开的小说路径: 成功返回 true */
bool last_novel_load(char *buf, size_t size)
{
    if (!buf || size == 0) return false;

    nvs_handle_t handle;
    if (nvs_open(SETTINGS_NS, NVS_READONLY, &handle) != ESP_OK) return false;

    size_t len = size;
    esp_err_t ret = nvs_get_str(handle, LAST_NOVEL_KEY, buf, &len);
    nvs_close(handle);

    if (ret != ESP_OK || len == 0 || buf[0] == '\0') return false;

    strncpy(s_last_novel, buf, sizeof(s_last_novel) - 1);
    s_last_novel[sizeof(s_last_novel) - 1] = '\0';
    return true;
}

/* ─────────────────────── 小说阅读进度 ─────────────────────── */
/* key = "np" + 8 位十六进制(文件名去扩展名后的 32 位哈希), 长度 10 < NVS 上限 15.
 * 用文件名(而非完整路径)作键, 避免路径过长; 值 = 文件字节偏移 */

/* 由 path 取文件名(去扩展名)算出进度 key, 写入 out (至少 16 字节) */
static void novel_progress_key(const char *path, char *out, size_t out_size)
{
    const char *slash = strrchr(path, '/');
    const char *fname = slash ? slash + 1 : path;

    char name_key[160];                                  /* 去扩展名后的文件名 */
    song_hash_name_key(fname, name_key, sizeof(name_key));
    uint32_t h = song_hash32(name_key, strlen(name_key));

    snprintf(out, out_size, "np%08" PRIx32, h);
}

/* 保存小说阅读进度: path=小说路径, offset=文件字节偏移 */
void novel_progress_save(const char *path, uint32_t offset)
{
    if (!path || !path[0]) return;

    char key[16];
    novel_progress_key(path, key, sizeof(key));

    nvs_handle_t handle;
    if (nvs_open(SETTINGS_NS, NVS_READWRITE, &handle) != ESP_OK) return;

    esp_err_t ret = nvs_set_u32(handle, key, offset);
    if (ret == ESP_OK) ret = nvs_commit(handle);
    nvs_close(handle);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "已保存小说进度: %s -> %lu", path, (unsigned long)offset);
    } else {
        ESP_LOGW(TAG, "保存进度失败 (%s)", esp_err_to_name(ret));
    }
}

/* 读取小说阅读进度, 成功返回 true 并输出 offset */
bool novel_progress_load(const char *path, uint32_t *offset)
{
    if (!path || !path[0] || !offset) return false;

    char key[16];
    novel_progress_key(path, key, sizeof(key));

    nvs_handle_t handle;
    if (nvs_open(SETTINGS_NS, NVS_READONLY, &handle) != ESP_OK) return false;

    uint32_t v = 0;
    esp_err_t ret = nvs_get_u32(handle, key, &v);
    nvs_close(handle);

    if (ret != ESP_OK) return false;
    *offset = v;
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

/* ──────────────────────────── 应用模式 ──────────────────────────── */
#define APP_MODE_KEY   "app_mode"

/* 读上次应用模式: 无记录/越界返回 SETTINGS_APP_MODE_MUSIC */
int settings_app_mode_load(void)
{
    nvs_handle_t h;
    if (nvs_open(SETTINGS_NS, NVS_READONLY, &h) != ESP_OK) return SETTINGS_APP_MODE_MUSIC;

    int32_t m = SETTINGS_APP_MODE_MUSIC;
    esp_err_t ret = nvs_get_i32(h, APP_MODE_KEY, &m);
    nvs_close(h);

    if (ret == ESP_OK && (m == SETTINGS_APP_MODE_MUSIC || m == SETTINGS_APP_MODE_NOVEL)) return (int)m;
    return SETTINGS_APP_MODE_MUSIC;
}

/* 保存当前应用模式到 NVS */
void settings_app_mode_save(int mode)
{
    if (mode != SETTINGS_APP_MODE_MUSIC && mode != SETTINGS_APP_MODE_NOVEL) mode = SETTINGS_APP_MODE_MUSIC;

    nvs_handle_t h;
    if (nvs_open(SETTINGS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, APP_MODE_KEY, (int32_t)mode);
        nvs_commit(h);
        nvs_close(h);
    }
}
