#ifndef __LYRICS_H__
#define __LYRICS_H__

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 单首歌词最大条目数.
 * 实测: 匿名M(ピノキオピー) 内嵌 LRC 有 206 行(日文行 + 中文译行), 192 会截掉
 * 尾部 14 行; 256 留出余量. 数组随条目数走 12B/条, 192→256 只多 768B (PSRAM). */
#define LYRICS_MAX_ENTRIES 256

/* 一条带时间轴的歌词. text 指向 lyrics_parse 的输入缓冲 (原地 NUL 结尾), 生命周期随缓冲 */
typedef struct {
    uint32_t    t_ms;   /* 起始时间 (毫秒) */
    const char *text;   /* UTF-8 文本 (可能含同一行内的中英混排) */
    uint16_t    len;    /* 文本字节数 */
} lyric_entry_t;

/* 解析 LRC 风格歌词 (原地修改 text: 把每行结尾改成 NUL).
 * 支持:
 *   - 一行多个 [mm:ss.xx] 时间戳
 *   - [offset:±ms] 整体偏移
 *   - 过滤 [ti:]/[ar:]/[by:] 等元数据行
 *   - 过滤开头 3 秒内的 作词/作曲/编曲 等制作信息
 * 返回解析出的条目数 (已按时间升序排序). */
int lyrics_parse(char *text, lyric_entry_t *out, int max_entries);

#ifdef __cplusplus
}
#endif

#endif
