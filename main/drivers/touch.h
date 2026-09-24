#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 触摸 (CST816) 驱动: 无独立任务, 由 LVGL 50Hz 输入回调按需读取.
 * GPIO INT 为低脉冲 (下降沿触发 ISR 置标志); 仅在"有中断"或"当前判定按下"时
 * 读 I2C, 空闲时不碰总线 (芯片休眠时不去读, 规避 ESP32 原版 I2C FSM 卡死). */

void touch_init(void);                    /* I2C 总线 + INT 中断初始化 */
bool touch_poll(int32_t *x, int32_t *y);  /* 读触点; 返回是否按下, x/y=原始坐标 */
void touch_reset(void);                   /* 清触点/中断状态 (息屏用) */

#ifdef __cplusplus
}
#endif
