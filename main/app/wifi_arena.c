/* WiFi 专用私有堆 (实现)
 *
 * 三个关键实现选择, 都有出处:
 *
 * 1) 用的是 IDF 自己的 multi_heap, 不是另写一个分配器.
 *    multi_heap.h:11-15 写明 "Any contiguous block of memory can be registered as a
 *    heap"; multi_heap.h:98 的 multi_heap_register(start, size) 就吃一个裸指针.
 *    IDF 自带测试甚至专门验证过把 .bss 静态数组注册成堆
 *    (test_apps/heap_tests/main/test_runtime_heap_reg.c:71-78), 所以拿 LVGL 的
 *    静态绘制缓冲来建堆是正规用法, 不是取巧.
 *
 * 2) 锁用 portMUX (由 multi_heap_platform.h:16-35 决定), 不是互斥量.
 *    MULTI_HEAP_LOCK 展开是 portENTER_CRITICAL_SAFE —— 中断里也能进.
 *    传 NULL 等于完全不加锁 (multi_heap.c:157 默认 lock=NULL), 那是单任务环境的
 *    用法; WiFi 有 wifi 任务 + tcpip 任务在分配, 必须自己挂一把.
 *
 * 3) 关堆之后区域地址仍然留着 (s_base/s_len 不清).
 *    万一 WiFi 库里还有一块迟到的 free, 能靠地址认出"这本该是我的"而不是把
 *    指针交给 heap_caps_free (那会直接判非法释放). 区域反正马上还给 LVGL,
 *    漏掉的那块跟着一起还, 无害. */

#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "multi_heap.h"

#include "wifi_arena.h"

static const char *TAG = "wifi_arena";

static multi_heap_handle_t s_heap = NULL;
static uint8_t  *s_base = NULL;      /* 区域起址 (关堆后保留, 见文件头第 3 条) */
static size_t    s_len  = 0;         /* 区域长度 */

/* 区域是 .bss 里的静态数组, 但 multi_heap 的块头按 size_t 读写, 所以锁本身
 * 用 portMUX 的静态初始化即可 (不占堆) */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static uint32_t s_peak   = 0;        /* 峰值占用 (含堆元数据) */
static uint32_t s_allocs = 0;        /* 成功分配次数 */
static uint32_t s_fails  = 0;        /* 堆满导致的失败次数 */
static uint32_t s_late   = 0;        /* 关堆之后才回来的 free */
static uint32_t s_spill  = 0;        /* *_internal 在私有堆里拿不到, 落到公共堆的次数 */

static inline bool in_arena(const void *p)
{
    if (!s_base) return false;
    uintptr_t a = (uintptr_t)p;
    return a >= (uintptr_t)s_base && a < (uintptr_t)s_base + s_len;
}

/* 占用 = 区域长度 - 剩余可分配字节 (multi_heap_free_size 是 O(1) 的计数器) */
static void stat_update(void)
{
    if (!s_heap) return;
    uint32_t used = (uint32_t)(s_len - multi_heap_free_size(s_heap));
    if (used > s_peak) s_peak = used;
}

bool wifi_arena_open(void *mem, size_t len)
{
    if (s_heap) return true;                     /* 幂等 */

    /* 32 字节的堆头 + tlsf 自己的位图, 太小的区域建了也是白建 */
    if (!mem || len < 1024) {
        ESP_LOGE(TAG, "区域不可用 (%p, %u B)", mem, (unsigned)len);
        return false;
    }

    s_base = (uint8_t *)mem;
    s_len  = len;
    s_heap = multi_heap_register(mem, len);
    if (!s_heap) {
        ESP_LOGE(TAG, "multi_heap_register 失败 (%u B)", (unsigned)len);
        s_base = NULL;
        s_len  = 0;
        return false;
    }
    multi_heap_set_lock(s_heap, &s_lock);

    s_peak = s_allocs = s_fails = s_late = s_spill = 0;
    ESP_LOGI(TAG, "私有堆就绪: %p + %u B, 可用 %u B",
             mem, (unsigned)len, (unsigned)multi_heap_free_size(s_heap));
    return true;
}

void wifi_arena_close(void)
{
    if (!s_heap) return;

    multi_heap_info_t info;
    multi_heap_get_info(s_heap, &info);

    /* 这一行就是"33KB 到底够不够"的答案: 峰值贴着容量说明下次要更大,
     * 失败次数非 0 说明已经有人拿到 NULL 了 */
    ESP_LOGI(TAG, "私有堆关闭: 峰值 %u/%u B (%u%%), 分配 %u 次, 失败 %u 次, 溢出 %u 次, 迟到 free %u",
             (unsigned)s_peak, (unsigned)s_len,
             (unsigned)(s_len ? (s_peak * 100u / s_len) : 0u),
             (unsigned)s_allocs, (unsigned)s_fails, (unsigned)s_spill, (unsigned)s_late);
    if (info.allocated_blocks) {
        /* 还有块没还: 内存马上还给 LVGL, 漏掉的那几块跟着一起还, 不单独回收 */
        ESP_LOGW(TAG, "仍有 %u 块未归还 (WiFi 库没释放), 随区域一起还给 LVGL",
                 (unsigned)info.allocated_blocks);
    }

    s_heap = NULL;      /* s_base/s_len 保留, 见文件头第 3 条 */
}

bool wifi_arena_active(void)
{
    return s_heap != NULL;
}

void *wifi_arena_malloc(size_t n)
{
    if (!s_heap) return NULL;
    void *p = multi_heap_malloc(s_heap, n);
    if (!p) {
        s_fails++;
        return NULL;
    }
    s_allocs++;
    stat_update();
    return p;
}

void *wifi_arena_calloc(size_t n, size_t sz)
{
    if (!s_heap) return NULL;
    if (sz && n > (size_t)-1 / sz) return NULL;     /* 乘法溢出 */
    size_t total = n * sz;
    void *p = wifi_arena_malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *wifi_arena_zalloc(size_t n)
{
    return wifi_arena_calloc(1, n);
}

void *wifi_arena_realloc(void *p, size_t n)
{
    if (!p) return wifi_arena_malloc(n);

    if (s_heap && in_arena(p)) {
        void *q = multi_heap_realloc(s_heap, p, n);
        if (!q) {
            s_fails++;
            return NULL;
        }
        stat_update();
        return q;
    }
    /* 不在私有区: 只能是别的组从公共堆拿的, 按原样交给公共堆 */
    return heap_caps_realloc(p, n, MALLOC_CAP_DEFAULT);
}

/* ── _*_internal 那组: 两级分配 (私有堆优先, 满了落公共堆) ──
 *
 * 为什么是两级而不是只用私有堆: RX 缓冲每块 1600 B, 数量按配置走, 而私有堆只有
 * 33KB (可用 ~29.8KB) —— 加缓冲时很撑. 公共 DMA 堆那边还有十几 KB 一直闲着
 * (lwIP/TLS 用不完), 让它兜住溢出的部分, 就能把两边都用上.
 * 这不破坏"WiFi 不占公共堆"的初衷: 私有堆仍然是第一选择, 溢出次数会在关堆时打出来.
 *
 * 释放不需要区分: wifi_arena_free 本来就按地址判断 (在区内 → multi_heap, 否则 →
 * heap_caps_free), 所以两级分配天然对称. */
void *wifi_arena_malloc_internal(size_t n)
{
    if (s_heap) {
        void *p = multi_heap_malloc(s_heap, n);
        if (p) {
            s_allocs++;
            stat_update();
            return p;
        }
    }
    s_spill++;
    return heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void *wifi_arena_calloc_internal(size_t n, size_t sz)
{
    if (sz && n > (size_t)-1 / sz) return NULL;     /* 乘法溢出 */
    size_t total = n * sz;
    void *p = wifi_arena_malloc_internal(total);
    if (p) memset(p, 0, total);
    return p;
}

void *wifi_arena_zalloc_internal(size_t n)
{
    return wifi_arena_calloc_internal(1, n);
}

/* ── _malloc / _wifi_* 两组: 同样是两级, 只是回落到"通用堆" (可以落 PSRAM) ──
 *
 * 实测 0.7.32: 私有堆 99% 满时, 这两组有 68 次 malloc 直接拿到 NULL —— 那 33KB
 * 已经装不下现在的缓冲配置 (静态 RX 8 + 动态 RX 8 + 重排窗口 8), 而公共堆那边
 * 还闲着 19KB. 回落到 MALLOC_CAP_DEFAULT (= 原生 g_wifi_osi_funcs._malloc 的语义,
 * 内部优先、放不下可以到 PSRAM), 既不撑爆私有堆, 也不会让 WiFi 库拿到 NULL. */
static void *tiered_alloc(size_t n, uint32_t caps)
{
    if (s_heap) {
        void *p = multi_heap_malloc(s_heap, n);
        if (p) {
            s_allocs++;
            stat_update();
            return p;
        }
    }
    s_spill++;
    return heap_caps_malloc(n, caps);
}

void *wifi_arena_malloc_tiered(size_t n)
{
    return tiered_alloc(n, MALLOC_CAP_DEFAULT);
}

void *wifi_arena_calloc_tiered(size_t n, size_t sz)
{
    if (sz && n > (size_t)-1 / sz) return NULL;     /* 乘法溢出 */
    size_t total = n * sz;
    void *p = wifi_arena_malloc_tiered(total);
    if (p) memset(p, 0, total);
    return p;
}

void *wifi_arena_zalloc_tiered(size_t n)
{
    return wifi_arena_calloc_tiered(1, n);
}

void wifi_arena_free(void *p)
{
    if (!p) return;

    if (s_heap && in_arena(p)) {
        multi_heap_free(s_heap, p);
        return;
    }
    if (!s_heap && in_arena(p)) {
        /* 关堆之后才回来: 不能交给 heap_caps_free (它会判非法释放), 记一笔丢掉 */
        if (s_late++ == 0) ESP_LOGW(TAG, "关堆之后仍有 free 落到私有区, 已忽略");
        return;
    }
    /* _malloc_internal 那组 (RX 缓冲等) 本来就是公共堆的, 还回去 */
    heap_caps_free(p);
}

/* ── libc free 的拦截 ──
 *
 * 必须拦: 闭源 WiFi 库里有一处**绕过 osi 表**直接调 libc free 的代码 ——
 * libpp.a 的 hal_crypto.o, 函数 hal_crypto_set_key_entry 里 `callx8 free`
 * (由 build/_ppobj 反汇编确认; 全库只有这一处 U free). 正常分配全走 osi 表的
 * _malloc, 而这一处走 libc free —— 原生 IDF 里两者是同一个函数, 所以库作者这么写
 * 没问题; 我们把 _malloc 换到私有堆之后, libc free 拿到私有堆指针就会判非法释放.
 *
 * 解决: 链接时用 -Wl,--wrap=free (见 main/CMakeLists.txt), 全镜像里对 free 的
 * 引用都改指向这里: 落私有区的交给 multi_heap, 其余原样交给 __real_free.
 * 对普通指针没有语义变化, 只多两次地址比较. */
void __real_free(void *p);

void __wrap_free(void *p)
{
    wifi_arena_free(p);
}
