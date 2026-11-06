#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* OTA 校验窗口 + 槽位诊断
 *
 * 机制完全依赖 IDF 自带的 otadata 状态, 不另外记标记:
 *   升级时 esp_ota_set_boot_partition() 写 NEW
 *   → 下次启动 bootloader 改成 PENDING_VERIFY
 *   → 所以运行中看到 PENDING_VERIFY 就等价于"这是 OTA 换上来的镜像, 且它
 *     还没被确认过, 这是它的第一次启动"。
 *   没确认就断电/崩溃 → bootloader 下次启动把它改成 ABORTED 并退回另一个槽。
 *
 * 全量烧录 (erase-flash) 后 otadata 是空的, bootloader 会直接写 VALID,
 * 所以出厂/烧录机出来的设备永远不会弹校验窗口。 */

/* 启动时调用 (app_main 里 "系统就绪" 之后) */
void ota_init(void);

/* LVGL 任务每帧调用一次: 遮罩在这里创建 (保证在 LVGL 上下文),
 * 同时兼作"UI 还活着"的判据 —— UI 卡死就不打标, 断电后会自然退回。 */
void ota_notify_ui_alive(void);

/* 是否处于校验窗口 (期间禁止睡眠、屏蔽切换槽位) */
bool ota_verifying(void);

/* 诊断: 打印运行/启动/下一升级槽 + 两个槽的版本与状态 */
void ota_report(void);

/* 切到指定槽并重启: slot 0 -> ota_0, 1 -> ota_1.
 * 镜像无效 (没烧/烧坏) 时 esp_ota_set_boot_partition 会直接拒绝, 不会重启。 */
esp_err_t ota_switch(int slot);

#ifdef __cplusplus
}
#endif
