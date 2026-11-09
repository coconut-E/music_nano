#ifndef __UI_SETTINGS_H__
#define __UI_SETTINGS_H__

#ifdef __cplusplus
extern "C" {
#endif

/* 设置页 (APP_MODE_SETTINGS): 白底整屏 lv_list.
 *
 * 现在列表里只有一项「固件版本」: 点进去是详情页, 显示当前运行 APP 的版本号
 * 和镜像大小, 下面一个「检查更新」按钮 —— 按需求先占位, 点了没有任何反应.
 *
 * 和 ui_menu.c 一样, 本模块只负责"建/删这两页"; 模式进出、资源清理与恢复
 * 全部由 app_mode.c 的切换流程负责 (与音乐/小说/菜单同一套), 设置永不写 NVS. */

void ui_settings_build(void);      /* 进入设置时调用: 建列表页 + 详情页 (详情默认隐藏) */
void ui_settings_destroy(void);    /* 离开设置时调用: 删两页并清句柄 */

#ifdef __cplusplus
}
#endif

#endif
