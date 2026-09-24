#ifndef __LIKES_H__
#define __LIKES_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 喜欢的歌曲 (NVS 命名空间 "likes")
 *
 * 分桶: bucket = hash % 10, 键名 "0".."9".
 * 每个键存一个 BLOB, 布局:
 *   [16 x uint32 LE] [8 字节全零] [16 x uint32 LE] [8 字节全零] ... [尾部 <16 个]
 *   - 分隔符只出现在完整组之间, 尾部 (可能不满 16 个) 不加分隔符
 *   - 解析时以 8 字节全零为分隔符切段, 逐段校验:
 *       中间段 (两侧都有分隔符) 必须恰好 16 个哈希 (64 字节);
 *       头/尾段允许不满 16 个, 但须为 4 字节 (32 bit) 整数倍.
 *   - 某段不合法只丢弃该段, 其余段照常保留 (不整桶丢弃)
 *
 * 用 BLOB 而非字符串: 分隔符含 '\0', nvs_set_str 会截断.
 * 键固定 10 个, 无需遍历, 直接 nvs_get_blob/set_blob.
 */

void likes_init(void);                  /* 打开命名空间 (幂等; 失败则功能降级) */
bool likes_contains(uint32_t h);        /* 是否已喜欢 */
bool likes_add(uint32_t h);             /* 标记喜欢, 返回是否新增 */
bool likes_remove(uint32_t h);          /* 取消喜欢, 返回是否删除 */

#ifdef __cplusplus
}
#endif

#endif
