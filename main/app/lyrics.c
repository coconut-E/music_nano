#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdbool.h>
#include "lyrics.h"

/* 解析 [mm:ss.xx] 内的内容 (不含方括号) */
static bool parse_tag_time(const char *tag, size_t len, uint32_t *out_ms)
{
    size_t i = 0;
    uint32_t minutes = 0;

    if (len == 0 || !isdigit((unsigned char)tag[i])) return false;
    while (i < len && isdigit((unsigned char)tag[i])) {
        minutes = minutes * 10 + (uint32_t)(tag[i] - '0');
        i++;
    }
    if (i >= len || tag[i] != ':') return false;
    i++;

    uint32_t seconds = 0;
    int sdig = 0;
    while (i < len && isdigit((unsigned char)tag[i]) && sdig < 2) {
        seconds = seconds * 10 + (uint32_t)(tag[i] - '0');
        i++;
        sdig++;
    }
    if (sdig == 0) return false;

    uint32_t frac = 0;
    int fdig = 0;
    if (i < len && (tag[i] == '.' || tag[i] == ':')) {
        i++;
        while (i < len && isdigit((unsigned char)tag[i]) && fdig < 3) {
            frac = frac * 10 + (uint32_t)(tag[i] - '0');
            i++;
            fdig++;
        }
    }
    if (i != len) return false;

    if (fdig == 1) frac *= 100;
    else if (fdig == 2) frac *= 10;

    *out_ms = (minutes * 60u + seconds) * 1000u + frac;
    return true;
}

/* 忽略大小写的前缀匹配 */
static bool starts_with_ci(const char *s, const char *prefix)
{
    size_t n = strlen(prefix);
    return strncasecmp(s, prefix, n) == 0;
}

/* 制作信息行: 只在开头 3 秒内过滤, 避免误伤正常歌词 */
static bool is_credit_line(const char *t)
{
    static const char *cn[] = {
        "制作人", "作词", "作曲", "编曲", "制作", "出品", "监制",
        "混音", "母带", "录音", "和声", "配唱", NULL
    };
    static const char *en[] = {
        "lyrics", "composed", "produced", "written", "music",
        "arranged", "mixed", "mastered", "producer", "songwriter", NULL
    };

    for (int i = 0; cn[i]; i++) {
        size_t n = strlen(cn[i]);
        if (strncmp(t, cn[i], n) == 0) {
            const char *p = t + n;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == ':' || *p == '：') return true;
        }
    }
    for (int i = 0; en[i]; i++) {
        size_t n = strlen(en[i]);
        if (starts_with_ci(t, en[i])) {
            const char *p = t + n;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == ':' || *p == '：') return true;
            if ((p[0] == 'b' || p[0] == 'B') &&
                (p[1] == 'y' || p[1] == 'Y') &&
                (p[2] == ' ' || p[2] == '\t')) return true;
        }
    }
    return false;
}

int lyrics_parse(char *text, lyric_entry_t *out, int max_entries)
{
    if (!text || !out || max_entries <= 0) return 0;

    /* UTF-8 BOM */
    if (strlen(text) >= 3 &&
        (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF) {
        memmove(text, text + 3, strlen(text + 3) + 1);
    }

    int count = 0;
    int32_t offset_ms = 0;
    char *p = text;

    while (*p) {
        char *e = strchr(p, '\n');
        if (e) *e = '\0';
        char *line = p;
        p = e ? (e + 1) : (p + strlen(p));

        size_t ll = strlen(line);
        if (ll && line[ll - 1] == '\r') line[--ll] = '\0';

        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        char *end = s + strlen(s);
        while (end > s && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
        if (!*s) continue;

        const char *q = s;
        uint32_t times[8];
        int nt = 0;

        while (*q == '[') {
            const char *close = strchr(q, ']');
            if (!close) break;
            size_t tl = (size_t)(close - q - 1);

            uint32_t t = 0;
            if (tl > 0 && isdigit((unsigned char)q[1]) &&
                parse_tag_time(q + 1, tl, &t)) {
                if (nt < (int)(sizeof(times) / sizeof(times[0]))) times[nt++] = t;
            } else if (tl > 7 && strncasecmp(q + 1, "offset:", 7) == 0) {
                const char *v = q + 8;
                int sign = 1;
                if (v < close && (*v == '+' || *v == '-')) {
                    if (*v == '-') sign = -1;
                    v++;
                }
                int32_t val = 0;
                while (v < close && isdigit((unsigned char)*v)) {
                    val = val * 10 + (*v - '0');
                    v++;
                }
                offset_ms = sign * val;
            }
            q = close + 1;
        }

        if (nt == 0) continue;

        const char *lyric = q;
        while (*lyric == ' ' || *lyric == '\t') lyric++;
        if (!*lyric) continue;

        if (times[0] < 3000u && is_credit_line(lyric)) continue;

        size_t len = strlen(lyric);
        if (len > 0xFFFFu) len = 0xFFFFu;

        for (int i = 0; i < nt && count < max_entries; i++) {
            int64_t t = (int64_t)times[i] + offset_ms;
            if (t < 0) t = 0;
            out[count].t_ms = (uint32_t)t;
            out[count].text = lyric;
            out[count].len  = (uint16_t)len;
            count++;
        }
    }

    /* 按时间升序 (稳定插入排序; 条目最多 192, 成本可忽略) */
    for (int i = 1; i < count; i++) {
        lyric_entry_t key = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].t_ms > key.t_ms) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = key;
    }

    return count;
}
