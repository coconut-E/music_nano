#ifndef __SONG_HASH_H__
#define __SONG_HASH_H__

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 歌曲哈希 (xxHash32, seed=0)
 * 说明: 当前工程实际只用 song_hash_name_key() (以"去扩展名的文件名"为键),
 * 未使用 song_hash_key() 的 "artist + '\x1f' + title" 键 (保留供参考/基准测试)。
 */

/* 计算 32 位哈希: data=数据, len=长度 */
uint32_t song_hash32(const void *data, size_t len);

/* 拼装歌曲键 "artist\x1f title" 到 out, 最多 cap-1 字符 (自动截断) */
void song_hash_key(const char *artist, const char *title, char *out, size_t cap);

/* 取文件名 (去掉最后一个 '.' 及其后的扩展名) 作为喜欢键写入 out */
void song_hash_name_key(const char *name, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
