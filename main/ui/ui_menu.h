#ifndef __UI_MENU_H__
#define __UI_MENU_H__

#ifdef __cplusplus
extern "C" {
#endif

/* 主菜单页 (APP_MODE_MENU): 三张图标竖排 —— 音乐 / 小说 / 设置.
 * 172x320 整屏放下, 无文字, 不滚动.
 *
 * 本模块只负责"建/删这一页"和"点了之后请求切到哪个模式";
 * 资源清理与恢复、NVS 写入全部由 app_mode.c 的切换流程负责 (与音乐/小说同一套). */

void ui_menu_build(void);      /* 进入菜单时调用: 构建整页 */
void ui_menu_destroy(void);    /* 离开菜单时调用: 删除整页并清句柄 */

#ifdef __cplusplus
}
#endif

#endif
