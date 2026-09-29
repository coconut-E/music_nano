#include "text_encoding.h"
#include "gbk_uni_table.h"
#include <string.h>

/*
 * 编码识别 + 转换实现.
 * GBK -> Unicode 码表来自 https://github.com/yeahlouis/GBK2UTF8 (MIT),
 * 本文件只复用其码表, 识别/边界/转换逻辑在此重新实现, 以正确处理:
 *   - 跨页截断的半个字符 (页码按原始字节推进, 不允许截断多字节字符)
 *   - BOM 剥离
 *   - 末尾不完整的 UTF-8 / GBK 序列 (留到下一页)
 */

#define GBK_LEAD_MIN   0x81
#define GBK_LEAD_MAX   0xFE
#define GBK_TRAIL_MIN  0x40
#define GBK_TRAIL_MAX  0xFE
#define GBK_TRAIL_BAD  0x7F

/* UTF-8 检查: 返回 1=完整合法, 2=合法但末尾字符不完整, 0=非法.
 * *prefix 输出可完整解析的前缀字节数 (不完整尾字符不计入). */
static int utf8_check(const uint8_t *s, size_t n, size_t *prefix)
{
    size_t i = 0;
    while (i < n) {
        uint8_t c = s[i];
        size_t need;
        uint32_t cp;

        if (c < 0x80) { i++; continue; }
        if ((c & 0xE0) == 0xC0) {
            if (c < 0xC2) return 0;              /* 过长编码 */
            need = 2;
        } else if ((c & 0xF0) == 0xE0) {
            need = 3;
        } else if ((c & 0xF8) == 0xF0) {
            if (c > 0xF4) return 0;              /* 超出 U+10FFFF */
            need = 4;
        } else {
            return 0;                            /* 非法首字节 (含续字节/0xFF) */
        }

        if (i + need > n) {                      /* 跨页截断: 视为合法, 尾字符留给下一页 */
            if (prefix) *prefix = i;
            return 2;
        }
        for (size_t k = 1; k < need; k++) {
            if ((s[i + k] & 0xC0) != 0x80) return 0;
        }
        if (need == 3) {
            cp = ((uint32_t)(c & 0x0F) << 12) |
                 ((uint32_t)(s[i + 1] & 0x3F) << 6) |
                 (uint32_t)(s[i + 2] & 0x3F);
            if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
        } else if (need == 4) {
            cp = ((uint32_t)(c & 0x07) << 18) |
                 ((uint32_t)(s[i + 1] & 0x3F) << 12) |
                 ((uint32_t)(s[i + 2] & 0x3F) << 6) |
                 (uint32_t)(s[i + 3] & 0x3F);
            if (cp < 0x10000 || cp > 0x10FFFF) return 0;
        }
        i += need;
    }
    if (prefix) *prefix = n;
    return 1;
}

/* GBK 检查: 返回 1=完整合法, 2=合法但末尾是孤立前导字节, 0=非法. */
static int gbk_check(const uint8_t *s, size_t n, size_t *prefix)
{
    size_t i = 0;
    while (i < n) {
        uint8_t c = s[i];
        if (c < 0x80) { i++; continue; }
        if (c >= GBK_LEAD_MIN && c <= GBK_LEAD_MAX) {
            if (i + 1 >= n) {                    /* 前导字节落在页尾: 留给下一页 */
                if (prefix) *prefix = i;
                return 2;
            }
            uint8_t t = s[i + 1];
            if (t < GBK_TRAIL_MIN || t > GBK_TRAIL_MAX || t == GBK_TRAIL_BAD) return 0;
            i += 2;
        } else {
            return 0;                            /* 0x80 / 0xFF 非法 */
        }
    }
    if (prefix) *prefix = n;
    return 1;
}

/* 往 out 写一个 Unicode 码点的 UTF-8 (BMP, 最多 3 字节); 空间不足返回 0 */
static int put_utf8(char *out, size_t cap, size_t *o, uint32_t cp)
{
    unsigned char u[3];
    int un;
    if (cp < 0x800) {
        u[0] = (unsigned char)(0xC0 | (cp >> 6));
        u[1] = (unsigned char)(0x80 | (cp & 0x3F));
        un = 2;
    } else {
        u[0] = (unsigned char)(0xE0 | (cp >> 12));
        u[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        u[2] = (unsigned char)(0x80 | (cp & 0x3F));
        un = 3;
    }
    if (*o + (size_t)un > cap) return 0;
    for (int k = 0; k < un; k++) out[(*o)++] = (char)u[k];
    return 1;
}

/* GBK -> UTF-8; 返回写入字节数. 结构性合法的双字节码若无映射则输出 '?' */
static size_t gbk_to_utf8(const uint8_t *s, size_t n, char *out, size_t cap)
{
    size_t o = 0, i = 0;
    while (i < n) {
        uint8_t c = s[i];
        if (c < 0x80) {
            if (o + 1 > cap) break;
            out[o++] = (char)c;
            i++;
            continue;
        }
        if (i + 1 < n) {
            uint8_t t = s[i + 1];
            if (c >= GBK_LEAD_MIN && c <= GBK_LEAD_MAX &&
                t >= GBK_TRAIL_MIN && t <= GBK_TRAIL_MAX && t != GBK_TRAIL_BAD) {
                unsigned idx = (unsigned)(c - GBK_LEAD_MIN) * 192u +
                               (unsigned)(t - GBK_TRAIL_MIN);
                if (idx < GBK_UNI_TABLE_LEN) {
                    uint32_t cp = gbk_uni_table[idx];
                    if (cp >= 0x80 && cp != 0x0001) {
                        if (!put_utf8(out, cap, &o, cp)) break;
                        i += 2;
                        continue;
                    }
                }
                /* 无映射: 占位符, 仍按 2 字节前进以免错位 */
                if (o + 1 > cap) break;
                out[o++] = '?';
                i += 2;
                continue;
            }
        }
        if (o + 1 > cap) break;
        out[o++] = '?';                          /* 孤立非法字节 */
        i++;
    }
    return o;
}

int text_to_utf8(const uint8_t *in, size_t n,
                 char *out, size_t cap, size_t *consumed)
{
    if (consumed) *consumed = n;
    if (!in || n == 0 || !out || cap == 0) return 0;

    size_t i = 0;   /* 跳过的 BOM 长度 */
    if (n >= 3 && in[0] == 0xEF && in[1] == 0xBB && in[2] == 0xBF) i = 3;

    size_t o = 0;

    size_t prefix = 0;
    int u = utf8_check(in + i, n - i, &prefix);
    if (u == 1 || u == 2) {
        size_t take = (u == 1) ? (n - i) : prefix;
        if (o + take > cap) take = cap - o;
        memcpy(out + o, in + i, take);
        o += take;
        if (consumed) *consumed = i + take;
    } else {
        int g = gbk_check(in + i, n - i, &prefix);
        if (g == 1 || g == 2) {
            size_t take = (g == 1) ? (n - i) : prefix;
            o = gbk_to_utf8(in + i, take, out, cap);
            if (consumed) *consumed = i + take;
        } else {
            /* 既非 UTF-8 也非 GBK: 原样拷贝 (保底, 至少 ASCII 可读) */
            size_t take = n - i;
            if (o + take > cap) take = cap - o;
            memcpy(out + o, in + i, take);
            o += take;
            if (consumed) *consumed = i + take;
        }
    }

    out[(o < cap) ? o : (cap - 1)] = '\0';
    return (int)o;
}
