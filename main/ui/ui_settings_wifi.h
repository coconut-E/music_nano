#ifndef __UI_SETTINGS_WIFI_H__
#define __UI_SETTINGS_WIFI_H__

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 设置页里的「连接 WiFi」子流程:
 *   固件版本页 --检查更新--> 连接WiFi(列表) --> 密码(横屏) --> 正在连接 --> 正在检查更新
 *
 * 本模块自己建/删这 4 个页面, 自己管这一次 WiFi 会话; ui_settings.c 只负责:
 *   1) 点「检查更新」时调 ui_wifi_enter(), 并把列表/详情页藏起来;
 *   2) 电源键短按时调 ui_wifi_back() (它自己决定 返回一级 / 拒绝 / 结束流程);
 *   3) 流程结束时通过 ui_wifi_set_end_cb() 收到回调, 把「固件版本」页显示回来;
 *   4) 离开设置模式时调 ui_wifi_destroy() 兜底 (含强制恢复竖屏).
 *
 * 线程: 所有函数只能在 LVGL 任务里调用. WiFi 相关操作一律走 wifi_mgr 的命令队列,
 * 由它自己的任务串行执行 —— 这里从不直接调 esp_wifi_*. */

/* 打开「连接 WiFi」页 (不会自动扫描). 已在流程里/上一次会话还没释放完 → false */
bool ui_wifi_enter(void);

/* 处理一次「返回」(电源键短按 / 页面左上角箭头). 扫描、连接、释放期间直接忽略 */
void ui_wifi_back(void);

/* 是否还停在 WiFi 流程的某一页 */
bool ui_wifi_active(void);

/* 强制收尾: 删页、停轮询、恢复竖屏、让 worker 释放 WiFi. 只给模式切换兜底用 */
void ui_wifi_destroy(void);

/* 流程结束回调 (回到「固件版本」页) */
void ui_wifi_set_end_cb(void (*cb)(void));

#ifdef __cplusplus
}
#endif

#endif
