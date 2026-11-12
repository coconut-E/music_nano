#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 显示屏 (JD9853, SPI).
 * 两段式初始化: lcd_init_early 在 app_main 头部 (建 SPI/面板 + SLPOUT, 非阻塞),
 * lcd_init_finish 在 lvgl 任务无限循环前 (距 SLPOUT≥120ms 后发寄存器命令 + DISPON).
 * 硬件复位由 bootloader 完成.
 * host: SPI 外设编号. 返回面板句柄. */
esp_lcd_panel_handle_t lcd_init_early(spi_host_device_t host);
esp_lcd_panel_handle_t lcd_init_finish(void);

/* 面板句柄 (LVGL 注册显示驱动用, 早于 lcd_init_finish) */
esp_lcd_panel_handle_t lcd_get_panel(void);

/* 同步刷屏: 整窗写入 color_data 并等待 DMA 传输排空后才返回 (color_data 可安全复用/释放).
 * 坐标同 esp_lcd_panel_draw_bitmap (x_end/y_end 为排他边界). */
esp_err_t lcd_draw_bitmap_sync(int x_start, int y_start, int x_end, int y_end,
                               const void *color_data);

/* 只等 SPI 在途事务排空 (DMA 屏障), 不动画面.
 * 竖屏刷屏是异步入队的, 复用/转交某块缓冲之前要它确认 DMA 已经不再读那块内存. */
esp_err_t lcd_wait_idle(void);

/* 背光亮度 (LEDC PWM, 0~255) */
void lcd_set_brightness(uint8_t level);

#ifdef __cplusplus
}
#endif
