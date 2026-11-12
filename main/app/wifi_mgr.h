#ifndef __WIFI_MGR_H__
#define __WIFI_MGR_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* WiFi 会话管理: 只给设置页的「检查更新」流程用.
 *
 * 为什么单独开一个任务
 * --------------------
 * esp_wifi_scan_start(阻塞式) 要 2~4 秒, esp_wifi_connect 等 IP 最多 20 秒.
 * 这些都不能在 LVGL 任务里跑, 否则整个界面 (含"扫描中…"的提示) 会卡死.
 * 所以这里开一个专属任务, 由它独占 WiFi 的初始化/扫描/连接/释放, UI 侧只投递
 * 命令、只取事件, 从不直接调 esp_wifi_*.
 *
 * 会话模型 (与"WiFi 操作不许中途退出, 用完必须全部释放"对应)
 * ---------------------------------------------------------
 *   一次会话 = 一个任务 + 一套 WiFi 资源.
 *   wifi_mgr_start_session() 起任务, 任务内做 ACQUIRE, 发 WIFI_EV_READY;
 *   之后串行处理 SCAN / CONNECT 命令 (一条跑完才取下一条, 没有并发, 也没有
 *   "中途取消"这种状态);
 *   收到 RELEASE 后按 断连 → stop → deinit → 删 netif → 删默认事件循环 →
 *   esp_netif_deinit 的顺序全部释放, 发 WIFI_EV_RELEASED, 然后任务自己退出.
 *   任务退出后不残留任何 WiFi 堆内存, 也不残留任务栈.
 *
 * 线程: 除 wifi_mgr_poll / wifi_mgr_flush_events / wifi_mgr_ap_* 之外的接口
 * 可以从任意任务调用 (内部只有队列投递); ap_* 只在收到 WIFI_EV_SCAN_DONE 后、
 * 发起下一次扫描前读, 与 UI 的用法天然互斥. */

/* 扫描结果最多保留多少个 (按 RSSI 从强到弱).
 * 取 5 = 屏幕一屏正好显示 5 行: 每建一行 LVGL 要 ~1KB 堆, 而 WiFi 自己正吃着
 * ~20KB 内部堆, 只留最强的 5 个既够用又不占内存. */
#define WIFI_MGR_MAX_APS   5

typedef enum {
    WIFI_EV_READY = 0,      /* 初始化 + STA 启动完成, 可以扫描了 */
    WIFI_EV_ERROR,          /* 初始化失败 (arg=0), 只能退出 */
    WIFI_EV_SCAN_DONE,      /* 扫描完成 (arg=扫描到的 AP 个数, 0 也合法) */
    WIFI_EV_SCAN_FAIL,      /* 扫描失败 (arg=esp_err_t) */
    WIFI_EV_CONNECTED,      /* 已连上并拿到 IP */
    WIFI_EV_CONNECT_FAIL,   /* 连接失败 (arg=断开原因码, 负数表示本地超时/参数错误) */
    WIFI_EV_UPDATE_CHECKED, /* 版本查询结束 (arg=update.h 的 upd_err_t; 0 表示查到了,
                             * 但"有没有新版本"要看 update_status()->phase == UPD_READY) */
    WIFI_EV_UPDATE_DONE,    /* 固件已写进备份槽并切好启动项, 可以重启了 */
    WIFI_EV_UPDATE_FAIL,    /* 下载/写入失败 (arg=upd_err_t), 槽位与 otadata 未动 */
    WIFI_EV_RELEASED,       /* 资源已全部释放, 任务已退出 */
} wifi_ev_t;

/* 把一块内存交给 WiFi 当私有堆用 (见 app/wifi_arena.h).
 *
 * 必须在 wifi_mgr_start_session() **之前**调: esp_wifi_init 在会话任务里一跑起来
 * 就开始分配, 那时堆必须已经建好, 否则这一轮还是从公共堆分.
 * mem/len 一般是 LVGL 让出来的第二块绘制缓冲 (ui_core_draw_buf_borrow).
 * 会话释放后这块内存仍归调用方, 由它还给 LVGL (ui_core_draw_buf_return). */
void wifi_mgr_provide_arena(void *mem, size_t len);

/* 开一次会话 (幂等: 已在会话中则什么都不做) */
void wifi_mgr_start_session(void);

/* 请求扫描: 结果留在本模块内, 完成后发 WIFI_EV_SCAN_DONE */
void wifi_mgr_scan(void);

/* 请求连接 (ssid/password 会被拷贝, 调用方可以立刻释放字符串) */
void wifi_mgr_connect(const char *ssid, const char *password);

/* 请求查版本 (GitHub Releases). 完成后发 WIFI_EV_UPDATE_CHECKED.
 * 只能在已连上 (WIFI_EV_CONNECTED) 之后投递. */
void wifi_mgr_update_check(void);

/* 请求下载并写入备份槽. 完成后发 WIFI_EV_UPDATE_DONE / WIFI_EV_UPDATE_FAIL.
 * 成功时 otadata 已经指向新槽, 由 UI 决定何时 esp_restart().
 * 进度不在这里推: UI 直接读 update_status() (见 app/update.h). */
void wifi_mgr_update_start(void);

/* 请求释放: 任务跑完手上的操作后释放全部资源并发 WIFI_EV_RELEASED */
void wifi_mgr_release(void);

/* 会话是否还活着 (任务未退出) */
bool wifi_mgr_session_active(void);

/* 取一条事件 (非阻塞): 有事件返回 true. ev/arg 可为 NULL */
bool wifi_mgr_poll(wifi_ev_t *ev, int *arg);

/* 丢掉残留事件 (重新进流程时调, 避免读到上一次会话的尾巴) */
void wifi_mgr_flush_events(void);

/* 扫描结果 (只在 WIFI_EV_SCAN_DONE 之后、下一次扫描之前有效) */
int         wifi_mgr_ap_count(void);
const char *wifi_mgr_ap_ssid(int idx);   /* 越界返回 "" */
bool        wifi_mgr_ap_open(int idx);   /* 开放网络 (不需要密码) */

#ifdef __cplusplus
}
#endif

#endif
