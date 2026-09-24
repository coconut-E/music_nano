#include <stdio.h>
#include <string.h>
#include "song_hash.h"

/* xxHash32 常量 (参考 xxHash 官方实现, seed 固定 0) */
#define PRIME32_1  0x9E3779B1U
#define PRIME32_2  0x85EBCA77U
#define PRIME32_3  0xC2B2AE3DU
#define PRIME32_4  0x27D4EB2FU
#define PRIME32_5  0x165667B1U

static inline uint32_t rotl32(uint32_t x, int r)
{
    return (x << r) | (x >> (32 - r));
}

/* 小端读取 4 字节 (与端序无关) */
static inline uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint32_t xxh_round(uint32_t acc, uint32_t input)
{
    return rotl32(acc + input * PRIME32_2, 13) * PRIME32_1;
}

uint32_t song_hash32(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    const uint8_t *const end = p + len;
    uint32_t h32;

    if (len >= 16) {
        const uint8_t *const limit = end - 16;
        uint32_t v1 = PRIME32_1 + PRIME32_2;
        uint32_t v2 = PRIME32_2;
        uint32_t v3 = 0;
        uint32_t v4 = (uint32_t)0 - PRIME32_1;
        do {
            v1 = xxh_round(v1, read_le32(p)); p += 4;
            v2 = xxh_round(v2, read_le32(p)); p += 4;
            v3 = xxh_round(v3, read_le32(p)); p += 4;
            v4 = xxh_round(v4, read_le32(p)); p += 4;
        } while (p <= limit);
        h32 = rotl32(v1, 1) + rotl32(v2, 7) + rotl32(v3, 12) + rotl32(v4, 18);
    } else {
        h32 = PRIME32_5;
    }

    h32 += (uint32_t)len;

    while (p + 4 <= end) {
        h32 += read_le32(p) * PRIME32_3;
        h32 = rotl32(h32, 17) * PRIME32_4;
        p += 4;
    }
    while (p < end) {
        h32 += (uint32_t)(*p) * PRIME32_5;
        h32 = rotl32(h32, 11) * PRIME32_1;
        p++;
    }

    h32 ^= h32 >> 15;
    h32 *= PRIME32_2;
    h32 ^= h32 >> 13;
    h32 *= PRIME32_3;
    h32 ^= h32 >> 16;
    return h32;
}

void song_hash_key(const char *artist, const char *title, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    if (!artist) artist = "";
    if (!title) title = "";

    /* artist + '\x1f' + title; 字段各 <=128, cap>=260 时不会截断 */
    snprintf(out, cap, "%s\x1f%s", artist, title);
}

void song_hash_name_key(const char *name, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    if (!name) name = "";

    /* 去掉最后一个 '.' 及其后的扩展名 (不以 '.' 开头时才截) */
    size_t n = strlen(name);
    const char *dot = strrchr(name, '.');
    if (dot && dot != name) n = (size_t)(dot - name);

    if (n > cap - 1) n = cap - 1;
    memcpy(out, name, n);
    out[n] = '\0';
}
