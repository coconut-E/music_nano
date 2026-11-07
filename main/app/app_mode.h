#ifndef __APP_MODE_H__
#define __APP_MODE_H__

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 应用模式 (四种, 编号即菜单从上到下的顺序):
 *   0 音乐 / 1 小说 / 2 设置 (占位, 未实现) / 3 主菜单.
 * 四组 UI 互斥: 切换时销毁旧组、构建新组, 切换过程由 power_mgr 用黑场过渡包裹.
 * 只有音乐(0)/小说(1) 会写进 NVS 作为下次启动模式; 设置和菜单永不落盘. */
typedef enum {
    APP_MODE_MUSIC    = 0,   /* 音乐模式 */
    APP_MODE_NOVEL    = 1,   /* 小说模式 */
    APP_MODE_SETTINGS = 2,   /* 设置模式: 占位, 菜单里留了入口但未实现 */
    APP_MODE_MENU     = 3,   /* 主菜单: 三图标页 (音乐/小说/设置) */
} app_mode_t;

/* main.c 在初始化子系统前读 NVS 并告知启动模式 */
void      app_mode_set_boot_mode(app_mode_t m);
app_mode_t app_mode_read_saved(void);   /* 读 NVS 上次模式 (默认音乐) */

/* UI 任务启动时调用: 按启动模式只构建并显示对应组 */
void app_mode_init(void);

/* 请求切换到目标模式 (长按睡眠键进菜单、菜单里点图标都走这里):
 * 目标 == 当前 时直接返回 (菜单里再长按就无反应);
 * 不同的模式之间用黑场过渡切换, 过渡中关触摸. */
void app_mode_request(app_mode_t target);

app_mode_t app_mode_get(void);          /* 当前模式 */
bool       app_mode_is_novel(void);     /* 当前是否小说模式 */
bool       app_mode_is_menu(void);      /* 当前是否主菜单页 */

#ifdef __cplusplus
}
#endif

#endif
