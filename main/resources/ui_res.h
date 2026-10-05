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

#ifdef __cplusplus
}
#endif
