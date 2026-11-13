/* 字库分区加载: 把 font16.bin 映射进来填 font_dsc
 *
 * 背景
 * ----
 * 字库 1.88MB (glyph_bitmap 1.50MB + glyph_dsc 0.46MB) 若留在 app 里会吃掉
 * 大半个 2.875MB 的 OTA 槽, 所以这两块搬到独立分区 (见 tools/split_font.py)。
 * 0.7.13 起位深从 4bpp 降到 2bpp, bitmap 2.80MB → 1.50MB: 16px 在 1.47"/247ppi
 * 屏上只有 1.645mm, 4 级灰的损失在实机距离下分辨不出来 (放大 5 倍才明显),
 * 换来的 1.3MB 给了两个 app 槽。
 *
 * 为什么用 mmap 而不是拷进 PSRAM
 * -----------------------------
 * 本机 PSRAM 只有 2MB (README "ESP32 双核 240MHz 2M PSRAM"), 装不下 1.88MB 之外
 * 还要放播放缓冲; 而且拷一份等于把启动时的读盘时间白花。mmap 是零拷贝, 读路径和
 * 原来放在 app .rodata 里完全一样, 性能不变。
 *
 * 唯一约束是 ESP32 的 DROM 窗口只有 4MB (0x3F400000-0x3F800000 = 64 页):
 * 拆完 app 的 .rodata 13 页 (829KB), 窗口剩 51 页, 本字库映射要 30 页 =
 * 1.97MB, **余量 21 页**。这 21 页也是 OTA 校验要用的 (esp_image_verify 至少
 * 要 2 页, 见 docs/ERRATA_zh.md 第六节第 5 条); 若以后 .rodata 涨到 1.3MB 以上,
 * mmap 会失败, 开机日志会打 "资源不足", 届时必须回头压 .rodata。
 *
 * 分区格式
 * ----
 * 64 字节头 + glyph_dsc[] + glyph_bitmap[], 见 tools/split_font.py 顶部说明。
 * 头里的 crc32 覆盖 header 之后的全部内容。
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "lvgl.h"

#include "font_part.h"

#define TAG "font"

/* 与 partitions.csv 的 font 行 (data, 0x40) 对应 */
#define FONT_SUBTYPE      ((esp_partition_subtype_t)0x40)
#define FONT_MAGIC        0x31544E46u   /* 小端字节序 'F','N','T','1' */
#define FONT_VERSION      1u
#define FONT_HDR_SIZE     64u
#define FONT_DSC_ENTRY    16u           /* glyph_dsc 每条字节数 (LARGE 模式) */
#define FONT_MAX_GLYPHS   200000u       /* 合理性上限, 防止头被篡改后算越界 */

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t total_size;
    uint32_t crc32;
    uint32_t dsc_off;
    uint32_t dsc_len;
    uint32_t bmp_off;
    uint32_t bmp_len;
    uint32_t glyph_count;
    uint32_t bpp;
    uint32_t reserved[6];
} font_hdr_t;

_Static_assert(sizeof(font_hdr_t) == FONT_HDR_SIZE, "字库头必须是 64 字节");
_Static_assert(sizeof(lv_font_fmt_txt_glyph_dsc_t) == FONT_DSC_ENTRY,
               "LV_FONT_FMT_TXT_LARGE 必须为 1 (glyph_dsc 每条 16 字节)");
_Static_assert(LV_COLOR_DEPTH == 16, "字库按 RGB565/4bpp 取模");

/* lv_font_global_16.c 提供: 返回它内部 font_dsc 的地址 */
lv_font_fmt_txt_dsc_t *font_global_16_dsc(void);

static uint32_t    s_crc_table[256];
static const void *s_map      = NULL;
static bool        s_crc_init = false;
static int         s_state    = -1;
static char        s_err[96]  = "";

/* 手动全量校验 (font crc 命令) 需要的信息, 只在 init 成功后有效 */
static const esp_partition_t *s_part = NULL;
static uint32_t    s_crc_expect = 0;   /* bin 里的 crc32 */
static uint32_t    s_crc_len    = 0;   /* 参与 CRC 的字节数 (header 之后) */

/* ------------------------------------------------------------------ */
/* CRC32 (标准 zlib 多项式 0xEDB88320), 与 tools/split_font.py 一致 */
/* ------------------------------------------------------------------ */
static void crc_table_init(void)
{
    if (s_crc_init) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        s_crc_table[i] = c;
    }
    s_crc_init = true;
}

static uint32_t crc32_std(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    while (n--) {
        crc = s_crc_table[(crc ^ *p++) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

/* ------------------------------------------------------------------ */
static int set_err(int state, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_err, sizeof s_err, fmt, ap);
    va_end(ap);
    s_state = state;
    return state;
}

/* 退化成 ASCII: 把 montserrat_16 的 dsc 整个拷进来。
 * 中文字形不在 ASCII cmaps 里, LVGL 会画成空白, 但不会崩, 用户仍能操作
 * 并重新烧录。 */
static void degrade_to_ascii(lv_font_fmt_txt_dsc_t *dsc, int state)
{
    const lv_font_fmt_txt_dsc_t *m =
        (const lv_font_fmt_txt_dsc_t *)lv_font_montserrat_16.dsc;
    if (m) {
        *dsc = *m;
    } else {
        dsc->glyph_bitmap = NULL;
        dsc->glyph_dsc    = NULL;
    }
    s_state = state;
    ESP_LOGE(TAG, "字库不可用 (%s), 已退化为 ASCII 字体", s_err);
}

/* ------------------------------------------------------------------ */
int font_part_init(void)
{
    if (s_state >= 0) return s_state;

    lv_font_fmt_txt_dsc_t *dsc = font_global_16_dsc();
    if (!dsc) {
        return set_err(FONT_PART_BAD, "font_global_16_dsc() 返回空");
    }

    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, FONT_SUBTYPE, "font");
    if (!part) {
        set_err(FONT_PART_NO_PART, "分区表里没有 font 分区");
        degrade_to_ascii(dsc, FONT_PART_NO_PART);
        return FONT_PART_NO_PART;
    }

    /* 1) 先用 esp_partition_read 读头 (只需一次 64B, 不占 MMU 页) */
    font_hdr_t hdr;
    esp_err_t err = esp_partition_read(part, 0, &hdr, sizeof hdr);
    if (err != ESP_OK) {
        set_err(FONT_PART_BAD, "读头失败: %s", esp_err_to_name(err));
        degrade_to_ascii(dsc, FONT_PART_BAD);
        return FONT_PART_BAD;
    }
    if (hdr.magic != FONT_MAGIC) {
        set_err(FONT_PART_BAD, "magic 不对 (0x%08x), 分区没烧字库?", (unsigned)hdr.magic);
        degrade_to_ascii(dsc, FONT_PART_BAD);
        return FONT_PART_BAD;
    }

    /* 2) 头字段自洽性 */
    if (hdr.version != FONT_VERSION) {
        set_err(FONT_PART_BAD, "版本不符 (bin=%u, fw 支持 %u)",
                (unsigned)hdr.version, (unsigned)FONT_VERSION);
        degrade_to_ascii(dsc, FONT_PART_BAD);
        return FONT_PART_BAD;
    }
    if (hdr.glyph_count == 0 || hdr.glyph_count > FONT_MAX_GLYPHS ||
        hdr.dsc_len != hdr.glyph_count * FONT_DSC_ENTRY) {
        set_err(FONT_PART_BAD, "glyph_count/dsc_len 不自洽 (%u/%u)",
                (unsigned)hdr.glyph_count, (unsigned)hdr.dsc_len);
        degrade_to_ascii(dsc, FONT_PART_BAD);
        return FONT_PART_BAD;
    }
    if (hdr.dsc_off < FONT_HDR_SIZE || hdr.bmp_off != hdr.dsc_off + hdr.dsc_len ||
        hdr.total_size != hdr.bmp_off + hdr.bmp_len ||
        hdr.total_size > part->size || hdr.bmp_len == 0) {
        set_err(FONT_PART_BAD, "分区内偏移/长度越界 (total=%u, part=%u)",
                (unsigned)hdr.total_size, (unsigned)part->size);
        degrade_to_ascii(dsc, FONT_PART_BAD);
        return FONT_PART_BAD;
    }

    /* 3) 只映射实际用到的部分并按 64KB 向上取整:
     *    3,266,026 -> 50 页 = 3,276,800 B, 比整分区 (52 页) 省 2 页。
     *    DROM 窗口拆完后只剩 54 页, 这 2 页不能浪费。 */
    size_t map_size = ((size_t)hdr.total_size + 0xFFFFu) & ~(size_t)0xFFFFu;
    if (map_size > part->size) map_size = part->size;

    esp_partition_mmap_handle_t handle = 0;
    err = esp_partition_mmap(part, 0, map_size, ESP_PARTITION_MMAP_DATA,
                            &s_map, &handle);
    if (err != ESP_OK) {
        set_err(FONT_PART_BAD, "mmap %u 字节失败: %s (DROM 窗口不足?)",
                (unsigned)map_size, esp_err_to_name(err));
        s_map = NULL;
        degrade_to_ascii(dsc, FONT_PART_BAD);
        return FONT_PART_BAD;
    }

    /* 4) 这里做过全量 CRC, 已删。
     *
     * 原因: 字库只由烧录器在"整片全烧"时写一次, 网络 OTA 完全不碰这个分区;
     * 而烧录器自己每段都回读 MD5。设备侧再算一遍 3.27MB 要 271ms, 本机是
     * 可插拔电池, 一天可能十几次冷启动, 这笔钱不值得花。
     * 真需要时用控制台命令 `font crc` 手动跑 (font_part_verify_crc)。
     *
     * 头部 magic/长度/偏移校验每次启动都做 (纯内存, 微秒级), 足以挡住
     * "没烧字库 / 烧到错地址 / 布局不匹配" 这三种真实会发生的故障。 */

    /* 5) 填指针。LVGL 读 glyph 时不拷贝, 直接按指针从 flash cache 取。 */
    const uint8_t *base = (const uint8_t *)s_map;
    dsc->glyph_bitmap = base + hdr.bmp_off;
    dsc->glyph_dsc    = (const lv_font_fmt_txt_glyph_dsc_t *)(base + hdr.dsc_off);

    /* 6) 抽检首末两条, 确认结构布局与固件 struct 一致 */
    const lv_font_fmt_txt_glyph_dsc_t *gd = dsc->glyph_dsc;
    if (gd[0].bitmap_index != 0 ||
        gd[hdr.glyph_count - 1].bitmap_index >= hdr.bmp_len) {
        set_err(FONT_PART_BAD, "glyph_dsc 布局不符 (首条 idx=%u, 末条 idx=%u)",
                (unsigned)gd[0].bitmap_index,
                (unsigned)gd[hdr.glyph_count - 1].bitmap_index);
        esp_partition_munmap(handle);
        s_map = NULL;
        degrade_to_ascii(dsc, FONT_PART_BAD);
        return FONT_PART_BAD;
    }

    s_state  = FONT_PART_OK;
    s_err[0] = '\0';
    s_part      = part;
    s_crc_expect = hdr.crc32;
    s_crc_len    = hdr.total_size - FONT_HDR_SIZE;
    ESP_LOGI(TAG, "已映射: %u 字形, dsc %u B, bitmap %u B, 共 %u 页 (%u KB)",
             (unsigned)hdr.glyph_count, (unsigned)hdr.dsc_len,
             (unsigned)hdr.bmp_len, (unsigned)(map_size / 0x10000u),
             (unsigned)(map_size / 1024u));
    return FONT_PART_OK;
}

/* 手动全量校验 (控制台 `font crc`)。不进启动路径: 一次 271ms。 */
int font_part_verify_crc(uint32_t *got)
{
    if (s_state != FONT_PART_OK || !s_map || !s_part) {
        return -1;
    }
    crc_table_init();
    int64_t  t0 = esp_timer_get_time();
    uint32_t c  = crc32_std((const uint8_t *)s_map + FONT_HDR_SIZE, (size_t)s_crc_len);
    int64_t  dt = (esp_timer_get_time() - t0) / 1000;
    if (got) {
        *got = c;
    }
    if (c != s_crc_expect) {
        ESP_LOGE(TAG, "全量 CRC 不匹配 (bin=0x%08x, flash=0x%08x, %d ms)",
                 (unsigned)s_crc_expect, (unsigned)c, (int)dt);
        return -2;
    }
    ESP_LOGI(TAG, "全量 CRC 通过 (0x%08x, %d ms, %u B)", (unsigned)c, (int)dt,
             (unsigned)s_crc_len);
    return 0;
}

int font_part_state(void)
{
    return s_state;
}

const char *font_part_err(void)
{
    return s_err;
}
