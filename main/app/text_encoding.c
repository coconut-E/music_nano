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

/* Windows-1251 高半区 (0x80..0xFF) 的 Unicode 映射.
 * 0x98 在 CP1251 中未定义, 这里按空白处理, 避免输出控制字符.
 * 0xC0..0xFF 是连续的西里尔字母 U+0410..U+044F. */
static const uint16_t cp1251_high[128] = {
    0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021,
    0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B, 0x040F,
    0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x0098, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F,
    0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7,
    0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407,
    0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7,
    0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457,
    0x0410, 0x0411, 0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417,
    0x0418, 0x0419, 0x041A, 0x041B, 0x041C, 0x041D, 0x041E, 0x041F,
    0x0420, 0x0421, 0x0422, 0x0423, 0x0424, 0x0425, 0x0426, 0x0427,
    0x0428, 0x0429, 0x042A, 0x042B, 0x042C, 0x042D, 0x042E, 0x042F,
    0x0430, 0x0431, 0x0432, 0x0433, 0x0434, 0x0435, 0x0436, 0x0437,
    0x0438, 0x0439, 0x043A, 0x043B, 0x043C, 0x043D, 0x043E, 0x043F,
    0x0440, 0x0441, 0x0442, 0x0443, 0x0444, 0x0445, 0x0446, 0x0447,
    0x0448, 0x0449, 0x044A, 0x044B, 0x044C, 0x044D, 0x044E, 0x044F,
};

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

/* 把 UTF-8 长度裁到 max 字节, 且不切断多字节字符 */
static size_t utf8_clip_len(const uint8_t *p, size_t len, size_t max)
{
    if (len <= max) return len;
    size_t cut = max;
    while (cut > 0 && (p[cut] & 0xC0) == 0x80) cut--;
    return cut;
}

/* 往 out 写一个 Unicode 码点的 UTF-8 (最多 4 字节); 空间不足/非法码点返回 0 */
static int put_utf8(char *out, size_t cap, size_t *o, uint32_t cp)
{
    unsigned char u[4];
    int un;
    if (cp < 0x80) {
        u[0] = (unsigned char)cp;
        un = 1;
    } else if (cp < 0x800) {
        u[0] = (unsigned char)(0xC0 | (cp >> 6));
        u[1] = (unsigned char)(0x80 | (cp & 0x3F));
        un = 2;
    } else if (cp < 0x10000) {
        if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;   /* 孤立代理项 */
        u[0] = (unsigned char)(0xE0 | (cp >> 12));
        u[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        u[2] = (unsigned char)(0x80 | (cp & 0x3F));
        un = 3;
    } else if (cp <= 0x10FFFF) {
        u[0] = (unsigned char)(0xF0 | (cp >> 18));
        u[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
        u[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        u[3] = (unsigned char)(0x80 | (cp & 0x3F));
        un = 4;
    } else {
        return 0;
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

/* ─────────────────── 媒体标签文本 (ID3v2 / FLAC / WAV) ─────────────────── */

/* 过滤标签里不应显示的码点: NBSP 归一化为空格, 控制字符替换为空格 */
static uint32_t sanitize_cp(uint32_t cp)
{
    if (cp == 0x00A0 || cp == 0x0098) return ' ';
    /* 保留换行/回车: 内嵌歌词 (USLT) 依赖 \n 分行; 普通标题里出现换行也无害 */
    if (cp < 0x20 && cp != '\t' && cp != '\n' && cp != '\r') return ' ';
    if (cp == 0x7F) return ' ';
    return cp;
}

/* 无 BOM 的 UTF-16: 用零字节落在奇/偶位置判断 LE/BE (ASCII 文本最可靠) */
static int utf16_detect_be(const uint8_t *in, size_t n)
{
    int even0 = 0, odd0 = 0;
    for (size_t i = 0; i + 1 < n; i += 2) {
        if (in[i] == 0) even0++;
        if (in[i + 1] == 0) odd0++;
    }
    return even0 > odd0;
}

/* UTF-16 (LE/BE) → UTF-8; 遇到 16bit NUL 结束, 跳过 BOM/代理对 */
static size_t utf16_to_utf8(const uint8_t *in, size_t n, int be,
                            char *out, size_t cap)
{
    if (!out || cap == 0) return 0;

    size_t o = 0;
    for (size_t i = 0; i + 1 < n; i += 2) {
        uint32_t u = be ? (((uint32_t)in[i] << 8) | in[i + 1])
                        : ((uint32_t)in[i] | ((uint32_t)in[i + 1] << 8));
        if (u == 0) break;   /* UTF-16 字符串结束 */

        uint32_t cp = u;
        if (u >= 0xD800 && u <= 0xDBFF && i + 3 < n) {   /* 高代理 + 低代理 */
            uint32_t lo = be ? (((uint32_t)in[i + 2] << 8) | in[i + 3])
                             : ((uint32_t)in[i + 2] | ((uint32_t)in[i + 3] << 8));
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            } else {
                cp = 0xFFFD;
            }
        } else if (u >= 0xDC00 && u <= 0xDFFF) {   /* 孤立低代理 */
            cp = 0xFFFD;
        }

        if (cp == 0xFEFF) continue;   /* BOM */
        cp = sanitize_cp(cp);
        if (!put_utf8(out, cap, &o, cp)) break;
    }

    out[o < cap ? o : (cap - 1)] = '\0';
    return o;
}

/* CP1251 → UTF-8; 0x80..0xFF 查表, 其余按 ASCII */
static size_t cp1251_to_utf8(const uint8_t *in, size_t n, char *out, size_t cap)
{
    if (!out || cap == 0) return 0;

    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t cp = (in[i] < 0x80) ? in[i] : cp1251_high[in[i] - 0x80];
        if (cp == 0) break;
        cp = sanitize_cp(cp);
        if (!put_utf8(out, cap, &o, cp)) break;
    }

    out[o < cap ? o : (cap - 1)] = '\0';
    return o;
}

/* 统计 UTF-8 中的西里尔字母数 (用于判断 CP1251 是否可信) */
static int utf8_cyr_count(const char *s, size_t n)
{
    int count = 0;
    size_t i = 0;
    while (i < n) {
        uint8_t c = (uint8_t)s[i];
        uint32_t cp;
        size_t need;
        if (c < 0x80) { i++; continue; }
        if ((c & 0xE0) == 0xC0)      { need = 2; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { need = 3; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { need = 4; cp = c & 0x07; }
        else { i++; continue; }
        if (i + need > n) break;
        for (size_t k = 1; k < need; k++) cp = (cp << 6) | ((uint8_t)s[i + k] & 0x3F);
        if (cp >= 0x0400 && cp <= 0x04FF) count++;
        i += need;
    }
    return count;
}

/* 统计 CP1251 候选文本的大小写模式:
 * upper/lower=西里尔大写/小写字母数, max_trans=单个词内最多的大小写切换次数.
 * 正常俄文名字/单词最多在首字母发生一次切换; GBK 误读成 CP1251 时
 * 常出现 ПфСЗРЩ 这类大小写交替, max_trans 会明显偏大, 用来避免误判. */
static void utf8_cyr_case_stats(const char *s, size_t n,
                                int *upper, int *lower, int *max_trans)
{
    int prev = 0, word_trans = 0;
    *upper = *lower = *max_trans = 0;

    size_t i = 0;
    while (i < n) {
        uint8_t c = (uint8_t)s[i];
        uint32_t cp;
        size_t need;
        if (c < 0x80) { need = 1; cp = c; }
        else if ((c & 0xE0) == 0xC0)      { need = 2; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0)      { need = 3; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0)      { need = 4; cp = c & 0x07; }
        else { i++; continue; }

        if (need) {
            if (i + need > n) break;
            for (size_t k = 1; k < need; k++) cp = (cp << 6) | ((uint8_t)s[i + k] & 0x3F);
        }

        int kind = 0;
        if ((cp >= 0x0410 && cp <= 0x042F) || cp == 0x0401) kind = 1;   /* 大写 */
        else if ((cp >= 0x0430 && cp <= 0x044F) || cp == 0x0451) kind = 2;   /* 小写 */

        if (kind) {
            if (kind == 1) (*upper)++;
            else           (*lower)++;
            if (prev && prev != kind) word_trans++;
            prev = kind;
        } else {
            if (word_trans > *max_trans) *max_trans = word_trans;
            word_trans = 0;
            prev = 0;
        }
        i += need ? need : 1;
    }
    if (word_trans > *max_trans) *max_trans = word_trans;
}

/* 统计 UTF-8 中的 CJK/假名/谚文/全角/西里尔字符数.
 * GBK 解码结果只要有这些字符, 就认为 GBK 比 CP1251/Latin-1 更可信. */
static int utf8_script_score(const char *s, size_t n)
{
    int score = 0;
    size_t i = 0;
    while (i < n) {
        uint8_t c = (uint8_t)s[i];
        uint32_t cp;
        size_t need;
        if (c < 0x80) { i++; continue; }
        if ((c & 0xE0) == 0xC0)      { need = 2; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { need = 3; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { need = 4; cp = c & 0x07; }
        else { i++; continue; }
        if (i + need > n) break;
        for (size_t k = 1; k < need; k++) cp = (cp << 6) | ((uint8_t)s[i + k] & 0x3F);

        if ((cp >= 0x0400 && cp <= 0x04FF) ||     /* 西里尔 */
            (cp >= 0x3400 && cp <= 0x4DBF) ||     /* CJK 扩展 A */
            (cp >= 0x4E00 && cp <= 0x9FFF) ||     /* CJK 基本区 */
            (cp >= 0x3040 && cp <= 0x30FF) ||     /* 平假名/片假名 */
            (cp >= 0xAC00 && cp <= 0xD7AF) ||     /* 谚文 */
            (cp >= 0xFF00 && cp <= 0xFFEF)) {     /* 全角字符 */
            score++;
        }
        i += need;
    }
    return score;
}

/* 未指定编码 (ID3v1/FLAC/WAV) 或 ID3 enc=0:
 * UTF-8 → CP1251(正常俄文模式) → GBK → CP1251(兜底) → Latin-1.
 * 本库实测以 GBK 为主 (包括 GBK 内码的西里尔字母), 所以 GBK 优先;
 * 只有 CP1251 候选出现至少 2 个小写西里尔字母且大小写模式正常时才提前采用,
 * 避免把 GBK 中文误读成 CP1251, 也避免把 Café 误判成 Cafй. */
static size_t ansi_to_utf8(const uint8_t *in, size_t n, char *out, size_t cap)
{
    if (!out || cap == 0) return 0;
    out[0] = '\0';

    /* C 字符串语义: 遇到第一个 NUL 结束 */
    size_t len = 0;
    while (len < n && in[len] != 0) len++;
    n = len;
    if (n == 0) return 0;

    /* UTF-8 BOM */
    if (n >= 3 && in[0] == 0xEF && in[1] == 0xBB && in[2] == 0xBF) {
        in += 3;
        n -= 3;
    }
    if (n == 0) return 0;

    int has_high = 0;
    for (size_t i = 0; i < n; i++) {
        if (in[i] >= 0x80) { has_high = 1; break; }
    }
    if (!has_high) {   /* 纯 ASCII */
        if (n > cap - 1) n = cap - 1;
        memcpy(out, in, n);
        out[n] = '\0';
        return n;
    }

    size_t prefix = 0;
    if (utf8_check(in, n, &prefix) == 1) {   /* 合法 UTF-8 (含误标为 enc=0 的情况) */
        n = utf8_clip_len(in, n, cap - 1);
        memcpy(out, in, n);
        out[n] = '\0';
        return n;
    }

    /* CP1251 优先: 正常俄文单词/姓名最多一次大小写切换.
     * 这一步放在 GBK 前, 防止 Михаил 这类 CP1251 字节碰巧是合法 GBK 时被误判. */
    size_t o = cp1251_to_utf8(in, n, out, cap);
    int upper = 0, lower = 0, max_trans = 0;
    utf8_cyr_case_stats(out, o, &upper, &lower, &max_trans);
    /* 要求至少 2 个小写字母: 避免把 GBK 中文误读出的全大写 CP1251 片段当真 */
    if (lower >= 2 && upper + lower >= 3 && max_trans <= 1) return o;

    /* GBK: 中文/日文/全角, 以及 GBK 内码的西里尔字母 */
    if (gbk_check(in, n, &prefix) == 1) {
        o = gbk_to_utf8(in, n, out, cap);
        if (o > 0 && utf8_script_score(out, o) > 0) {
            out[o < cap ? o : (cap - 1)] = '\0';   /* gbk_to_utf8 不写结尾 NUL */
            return o;
        }
    }

    /* CP1251 兜底 (短词/全大写等不满足上面的模式) */
    o = cp1251_to_utf8(in, n, out, cap);
    if (utf8_cyr_count(out, o) >= 2) return o;

    /* Latin-1 兜底 (西欧重音字符等) */
    size_t lo = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t cp = sanitize_cp(in[i]);
        if (!put_utf8(out, cap, &lo, cp)) break;
    }
    out[lo < cap ? lo : (cap - 1)] = '\0';
    return lo;
}

int text_tag_to_utf8(const uint8_t *in, size_t n, int enc,
                     char *out, size_t cap)
{
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    if (!in || n == 0) return 0;

    /* 任何来源只要以 UTF-16 BOM 开头, 都按 UTF-16 处理
     * (WAV INFO/FLAC 注释偶尔会写 UTF-16, 但调用方不知道编码) */
    if (n >= 2 && ((in[0] == 0xFF && in[1] == 0xFE) ||
                   (in[0] == 0xFE && in[1] == 0xFF))) {
        enc = 1;
    }

    if (enc == 1) {   /* UTF-16 with BOM (ID3v2 规范要求 BOM, 但也兼容缺失) */
        if (n >= 2 && in[0] == 0xFF && in[1] == 0xFE)
            return (int)utf16_to_utf8(in + 2, n - 2, 0, out, cap);
        if (n >= 2 && in[0] == 0xFE && in[1] == 0xFF)
            return (int)utf16_to_utf8(in + 2, n - 2, 1, out, cap);
        return (int)utf16_to_utf8(in, n, utf16_detect_be(in, n), out, cap);
    }

    if (enc == 2) {   /* UTF-16BE; 某些文件仍带 BOM */
        if (n >= 2 && in[0] == 0xFF && in[1] == 0xFE)
            return (int)utf16_to_utf8(in + 2, n - 2, 0, out, cap);
        if (n >= 2 && in[0] == 0xFE && in[1] == 0xFF)
            return (int)utf16_to_utf8(in + 2, n - 2, 1, out, cap);
        return (int)utf16_to_utf8(in, n, 1, out, cap);
    }

    if (enc == 3) {   /* UTF-8 */
        const uint8_t *p = in;
        size_t len = n;
        if (len >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) {
            p += 3;
            len -= 3;
        }
        for (size_t i = 0; i < len; i++) {
            if (p[i] == 0) { len = i; break; }   /* 多值串只取第一个 */
        }
        size_t prefix = 0;
        if (utf8_check(p, len, &prefix) == 1) {
            len = utf8_clip_len(p, len, cap - 1);
            memcpy(out, p, len);
            out[len] = '\0';
            return (int)len;
        }
        return (int)ansi_to_utf8(p, len, out, cap);
    }

    /* enc=0 或未指定 (FLAC/WAV/ID3v1) */
    return (int)ansi_to_utf8(in, n, out, cap);
}
