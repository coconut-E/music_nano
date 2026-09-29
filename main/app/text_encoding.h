#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 文本编码自动识别 + 转 UTF-8 (LVGL 只支持 UTF-8).
 *
 * 每页独立识别, 不做持久化:
 *   - 开头 UTF-8 BOM (EF BB BF) 会被剥离 (LVGL 字体无 U+FEFF 字形, 否则显示方框);
 *   - 整段可解析为 UTF-8 (允许末尾半个字符, 用于跨页截断) → 原样输出;
 *   - 否则若为合法 GBK → 用码表转为 UTF-8;
 *   - 都不是 → 原样拷贝 (保底).
 *
 * 入参:  in/n        原始字节
 * 出参:  out/cap     UTF-8 输出缓冲
 *        *consumed   实际消耗的原始字节数 (已对齐到完整字符边界, 含被剥离的 BOM),
 *                    调用方用 offset + consumed 作为下一页起始, 保证不丢/不重字
 * 返回:  写入 out 的 UTF-8 字节数 (不含结尾 '\0'), 失败返回 0
 */
int text_to_utf8(const uint8_t *in, size_t n,
                 char *out, size_t cap, size_t *consumed);

#ifdef __cplusplus
}
#endif
