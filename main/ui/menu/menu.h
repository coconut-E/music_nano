#ifndef __MENU_H__
#define __MENU_H__

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"
#include "bt_a2dp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── 面板开合动画 (menu_anim.c) ── */
/* 动画图实际显示位置 (与真实面板渲染位置对齐) */
#define ANIM_FS_X      13   /* 文件面板动画 X */
#define ANIM_FS_Y      11   /* 文件面板动画 Y */
#define ANIM_BT_X      26   /* BT_X=13 + 偏移13 (实测微调) */
#define ANIM_BT_Y      11   /* BT_Y=3  + 偏移8  */

/* 面板动画配置 */
typedef struct {
    lv_obj_t **overlay_ref;   /* 指向 overlay 指针 (全屏透明按钮) */
    lv_obj_t **cont_ref;      /* 指向 cont 指针 (白色面板容器) */
    int         w, h;         /* 面板尺寸 */
    int         x, y;         /* 动画图显示位置 */
    bool        anchor_right; /* true=从右上角展开 (蓝牙), false=从左上角展开 (文件) */
    void      (*open_real)(void);   /* 真实打开面板 */
    void      (*close_real)(void);  /* 真实关闭面板 */
} panel_anim_cfg_t;

void panel_anim_open(const panel_anim_cfg_t *cfg);   /* 打开面板动画 */
void panel_anim_close(const panel_anim_cfg_t *cfg);  /* 关闭面板动画 */

/* ── 文件浏览器 (menu_browser.c) ── */
void fs_list_set_play_cb(void (*cb)(const char *group, const char *name));   /* 注册点击播放回调 */
bool fs_browser_precreate_step(int budget_us);        /* 分步预建面板 (true=未完成, budget_us<=0 一次做完) */
void fs_menu_click_cb(lv_event_t *e);                 /* 文件菜单入口点击事件 */
void fs_browser_on_sd_ready(void);                    /* SD 就绪 → 加载列表 */
void fs_browser_on_sd_remove(void);                   /* SD 拔出 → 清列表 */
void fs_browser_refresh(void);                        /* 刷新列表 */
void fs_browser_jump(void);                           /* 跳到当前播放歌曲 */

/* ── 蓝牙设备列表 (menu_bt.c) ── */
void bt_list_init(bt_a2dp_iface_t *iface);            /* 初始化蓝牙列表 */
void bt_menu_click_cb(lv_event_t *e);                 /* 蓝牙菜单入口点击事件 */
void bt_list_on_device_found(const char *name);       /* 发现设备 */
void bt_list_on_scan_done(void);                      /* 扫描完成 */
void bt_list_on_connected(const char *name);          /* 已连接 */
void bt_list_on_connect_failed(const char *name);     /* 连接失败 */
void bt_list_on_disconnected(void);                   /* 已断开 */
void bt_list_on_state_rsp(bt_state_t state, const char *name);  /* 状态应答 */

#ifdef __cplusplus
}
#endif

#endif
