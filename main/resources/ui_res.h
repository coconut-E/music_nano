#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 文件图标位图 (16x21 RGB565, 2 帧) */
extern const uint16_t icon_file[2][336];

/* 粉色爱心图标 (16x21 RGB565, 已喜欢文件用) */
extern const uint16_t icon_heart[336];

/* 小说(TXT)文件图标 (16x21 RGB565, 小说库文件用) */
extern const uint16_t icon_novel[336];

/* 主菜单三张图标 (88x88 RGB565, 已抠成圆形并合成到菜单底色 0x050505)
 * 由 tools/gen_menu_icons.py 生成, 顺序与菜单从上到下一致 */
extern const uint16_t icon_menu_music[7744];      /* 音乐 */
extern const uint16_t icon_menu_novel[7744];      /* 小说 */
extern const uint16_t icon_menu_settings[7744];   /* 设置 */

#ifdef __cplusplus
}
#endif
