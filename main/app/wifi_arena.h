#ifndef __WIFI_ARENA_H__
#define __WIFI_ARENA_H__

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* WiFi 专用私有堆: 一块"只有 WiFi 用"的内存, 由我们自己分配和回收.
 *
 * 为什么不用公共堆
 * ----------------
 * 公共堆是共享的, 这块内存交还回去之后可能被任何模块切走; 下一次 WiFi 会话
 * 能不能再拿到同样大小、同样连续的一块, 没有保证. 私有堆的区域边界由调用方钉死,
 * 每次会话从空堆开始, 峰值过后所有块合并回一整块, 不存在长期碎片.
 * multi_heap.h:91 明确允许"块全空闲后直接另作他用", 所以同一块内存可以在
 * LVGL 和私有堆之间反复交接, 不需要注销.
 *
 * 谁用它
 * ------
 * wifi_mgr.c 把 wifi_osi_funcs_t 里的 _malloc/_calloc/_realloc/_zalloc/_free
 * 换成这里的函数, WiFi 的通用分配就全落在这一块里 (换表细节见 wifi_mgr.c).
 * _malloc_internal 那组 (RX 缓冲等必须是内部 RAM 的 DMA 分配) 不接, 留在公共堆. */

/* 建堆: mem/len 是一段连续、且没有别人在用的内存 (通常是 LVGL 让出的绘制缓冲).
 * 幂等: 已经建过就直接返回 true. 返回 false 表示这块地太小或用不了. */
bool wifi_arena_open(void *mem, size_t len);

/* 拆堆: 打印峰值/失败次数, 之后这块内存归调用方 (还给 LVGL).
 * 不释放任何东西 —— 区域本来就属于调用方, 堆元数据会被后续使用者覆盖. */
void wifi_arena_close(void);

/* 堆是否已建好 (wifi_mgr 用它决定是否换表) */
bool wifi_arena_active(void);

/* 下面这组直接就是 WiFi 分配表里对应字段的实现 (wifi_os_adapter.h:73-74 / :122-125):
 *
 *   _malloc / _free                        → wifi_arena_malloc / _free
 *   _malloc_internal / _realloc_internal   → 同上那三个 *_internal 用的包装
 *   _calloc_internal / _zalloc_internal
 *
 * 为什么 *_internal 那组也要接: 第一版只接了 _malloc/_free, 实测整轮流程
 * **一次都没被调用** (峰值 0 字节) —— WiFi 的大块分配全在 *_internal (RX 缓冲 /
 * mgmt / TX, init 时约 17.6KB) 和 _wifi_* 里. 不接这组, 私有堆就是白给. */
void *wifi_arena_malloc(size_t n);
void *wifi_arena_calloc(size_t n, size_t sz);
void *wifi_arena_zalloc(size_t n);
void *wifi_arena_realloc(void *p, size_t n);
void  wifi_arena_free(void *p);

/* _*_internal 那组用: 私有堆优先, 拿不到就落公共堆 (两级).
 * 私有堆只有 33KB, 而 RX 缓冲的数量是按配置走的; 公共 DMA 堆那边常年闲着十几 KB,
 * 让它兜住溢出, 加缓冲时才不会把那 33KB 撑爆. 释放走同一个 wifi_arena_free,
 * 它按地址分流, 所以不需要区分这两级. */
void *wifi_arena_malloc_internal(size_t n);
void *wifi_arena_calloc_internal(size_t n, size_t sz);
void *wifi_arena_zalloc_internal(size_t n);

/* _malloc / _wifi_* 两组用的两级分配: 私有堆优先, 满了落通用堆 (MALLOC_CAP_DEFAULT).
 * 0.7.32 实测私有堆 99% 满时这两组有 68 次拿到 NULL, 加这个回落是为了不再失败. */
void *wifi_arena_malloc_tiered(size_t n);
void *wifi_arena_calloc_tiered(size_t n, size_t sz);
void *wifi_arena_zalloc_tiered(size_t n);

#ifdef __cplusplus
}
#endif

#endif
