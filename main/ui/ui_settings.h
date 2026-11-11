#ifndef __UI_SETTINGS_H__
#define __UI_SETTINGS_H__

#ifdef __cplusplus
extern "C" {
#endif

/* 设置页 (APP_MODE_SETTINGS): 白底整屏 lv_list.
 *
 * 列表里目前只有一项「固件版本」: 点进去是详情页, 显示当前运行 APP 的版本号
 * 和镜像大小, 下面一个「检查更新」按钮 —— 它进入 WiFi 子流程
 * (连接 WiFi → 密码 → 正在连接 → 正在检查更新), 那几页在 ui_settings_wifi.c 里.
 *
 * 和 ui_menu.c 一样, 本模块只负责"建/删这些页面"; 模式进出、资源清理与恢复
 * 全部由 app_mode.c 的切换流程负责 (与音乐/小说/菜单同一套), 设置永不写 NVS. */

void ui_settings_build(void);      /* 进入设置时调用: 建列表页 + 详情页 (详情默认隐藏) */
void ui_settings_destroy(void);    /* 离开设置时调用: 先收 WiFi 子流程, 再删两页 */

/* 电源键短按: 返回上一级 (详情→列表); 已经在列表这一级则退出设置回主菜单.
 * 停在 WiFi 子流程里时转交给 ui_wifi_back() (扫描/连接/释放期间无反应).
 * 无操作自动熄屏不走这里 —— 它仍然按原来的逻辑睡/熄屏. */
void ui_settings_back(void);

#ifdef __cplusplus
}
#endif

#endif
