#ifndef __PLAYED_BITS_H__
#define __PLAYED_BITS_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 随机播放去重位图 (每个文件夹一条比特串, 存 NVS "pbits" 命名空间):
 *  - 比特位下标与 player_file_name_at() 对该文件夹的枚举顺序一致
 *  - 随机模式播放到某首即置 1; 随机只从 0 位中取
 *  - 全 1 时在下次取歌前清零 (懒清零)
 *  - 位图长度 = 该文件夹文件数, 插卡时全量校验, 不符则重置
 *  - 所有位图常驻 PSRAM, 运行时只写 NVS
 */

/* 插卡/缓存就绪: 全量校验 + 载入全部文件夹位图到 PSRAM */
void played_bits_on_sd_ready(void);

/* 拔卡: 释放 PSRAM 并关闭 NVS 句柄 */
void played_bits_on_sd_remove(void);

/* 在 group 的 0 位中随机取一个下标; 无可用返回 -1 */
int  played_bits_pick(const char *group);

/* 将 group 的第 idx 位置 1 并持久化; 失败返回 false */
bool played_bits_mark(const char *group, int idx);

/* 删除第 idx 个文件后修正位图: 丢弃 idx 位, 其后各位左移 1 位, valid_bits-1, 持久化 */
bool played_bits_remove_at(const char *group, int idx);

#ifdef __cplusplus
}
#endif

#endif
