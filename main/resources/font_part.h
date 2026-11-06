#pragma once

#include <stdint.h>

/* 字库分区加载 (main/resources/font16.bin -> 分区表里的 font 分区)
 *
 * app 里不再包含 glyph_bitmap / glyph_dsc (共 3.27MB), 开机时把 font 分区
 * 通过 MMU 映射进 DROM 窗口, 再把指针填进字库的 font_dsc。
 * 读路径和原来完全一样 (都是 flash cache 映射), 渲染性能不变。
 *
 * 必须在任何文字绘制之前调用 (main.c 里 nvs_flash_init() 之后)。
 * 失败时自动退化成 ASCII 字体 (lv_font_montserrat_16 的数据):
 * 中文显示为空白但不崩溃, 屏幕顶栏会由 UI 提示 "FONT PARTITION INVALID"。
 */

#define FONT_PART_OK        0   /* 映射并校验通过 */
#define FONT_PART_NO_PART   1   /* 分区表里没有 font 分区 (旧固件 / 没烧字库) */
#define FONT_PART_BAD       2   /* 分区在, 但头/长度/偏移校验不过 */

int font_part_init(void);
int font_part_state(void);

/* 失败原因 (简短中文串), 成功时为空串 */
const char *font_part_err(void);

/* 手动全量 CRC (控制台 `font crc`), 返回 0 通过 / -1 字库未就绪 / -2 不匹配。
 * 刻意不进启动路径: 3.27MB 一次要 271ms, 而本机是可插拔电池, 冷启动很多。
 * 字库只由烧录器全量烧写时写入, 网络 OTA 不碰它。 */
int font_part_verify_crc(uint32_t *got);
