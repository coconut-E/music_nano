#ifndef __APP_MODE_H__
#define __APP_MODE_H__

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 应用模式: 音乐播放器 / 小说阅读器.
 * 两组 UI 互斥: 切换时销毁旧组、构建新组, 切换过程由 power_mgr 用黑屏过渡包裹. */
typedef enum {
    APP_MODE_MUSIC = 0,   /* 音乐模式 */
    APP_MODE_NOVEL = 1,   /* 小说模式 */
} app_mode_t;

/* main.c 在初始化子系统前读 NVS 并告知启动模式 */
void      app_mode_set_boot_mode(app_mode_t m);
app_mode_t app_mode_read_saved(void);   /* 读 NVS 上次模式 (默认音乐) */

/* UI 任务启动时调用: 按启动模式只构建并显示对应组 */
void app_mode_init(void);

/* 长按睡眠键触发: 用黑屏过渡切换模式 */
void app_mode_toggle(void);

app_mode_t app_mode_get(void);          /* 当前模式 */
bool       app_mode_is_novel(void);     /* 当前是否小说模式 */

#ifdef __cplusplus
}
#endif

#endif
