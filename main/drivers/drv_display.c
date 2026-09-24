#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_jd9853.h"
#include "drv_display.h"

static const char *TAG = "drv_display";

static void lcd_backlight_init(void);

/* ────────────────────────── LCD (JD9853) ────────────────────────── */
/* SPI/控制引脚定义 */
#define PIN_LCD_SCLK  18   /* SPI 时钟 */
#define PIN_LCD_MOSI  23   /* SPI 数据输出 (写命令/像素) */
#define PIN_LCD_CS     5   /* 片选 (低有效) */
#define PIN_LCD_DC    32   /* 数据/命令选择: 高=数据, 低=命令 */
#define PIN_LCD_RST_HW 25  /* 复位实际引脚 (低有效, 脉冲由 bootloader 钩子完成并保持高) */
#define PIN_LCD_RST   -1   /* 传给面板驱动: -1 = 驱动不接管该脚, 否则会把 GPIO25 配成输出低而把面板按在复位 */
#define PIN_LCD_BL     4   /* 背光 PWM 引脚 */

#define LCD_H_RES  172   /* 屏水平分辨率 (像素) */
#define LCD_V_RES  320   /* 屏垂直分辨率 (像素) */

/* JD9853 上电初始化寄存器序列 (厂家推荐值, 由 esp_lcd_jd9853 组件执行):
 * 每项 = {命令码, 参数数组, 参数个数, 等待ms} */
static const jd9853_lcd_init_cmd_t init_cmds[] = {
    {0xDF, (uint8_t[]){0x98, 0x53}, 2, 0},
    {0xDF, (uint8_t[]){0x98, 0x53}, 2, 0},
    {0xB2, (uint8_t[]){0x23}, 1, 0},
    {0xB7, (uint8_t[]){0x00, 0x47, 0x00, 0x6F}, 4, 0},
    {0xBB, (uint8_t[]){0x1C, 0x1A, 0x55, 0x73, 0x63, 0xF0}, 6, 0},
    {0xC0, (uint8_t[]){0x44, 0xA4}, 2, 0},
    {0xC1, (uint8_t[]){0x16}, 1, 0},
    {0xC3, (uint8_t[]){0x7D, 0x07, 0x14, 0x06, 0xCF, 0x71, 0x72, 0x77}, 8, 0},
    {0xC4, (uint8_t[]){0x00, 0x00, 0xA0, 0x79, 0x0B, 0x0A, 0x16, 0x79, 0x0B, 0x0A, 0x16, 0x82}, 12, 0},
    {0xC8, (uint8_t[]){0x3F, 0x32, 0x29, 0x29, 0x27, 0x2B, 0x27, 0x28, 0x28, 0x26, 0x25, 0x17, 0x12, 0x0D, 0x04, 0x00,
                       0x3F, 0x32, 0x29, 0x29, 0x27, 0x2B, 0x27, 0x28, 0x28, 0x26, 0x25, 0x17, 0x12, 0x0D, 0x04, 0x00}, 32, 0},
    {0xD0, (uint8_t[]){0x04, 0x06, 0x6B, 0x0F, 0x00}, 5, 0},
    {0xD7, (uint8_t[]){0x00, 0x30}, 2, 0},
    {0xE6, (uint8_t[]){0x14}, 1, 0},
    {0xDE, (uint8_t[]){0x01}, 1, 0},
    {0xB7, (uint8_t[]){0x03, 0x13, 0xEF, 0x35, 0x35}, 5, 0},
    {0xC1, (uint8_t[]){0x14, 0x15, 0xC0}, 3, 0},
    {0xC2, (uint8_t[]){0x06, 0x3A}, 2, 0},
    {0xC4, (uint8_t[]){0x72, 0x12}, 2, 0},
    {0xBE, (uint8_t[]){0x00}, 1, 0},
    {0xDE, (uint8_t[]){0x02}, 1, 0},
    {0xE5, (uint8_t[]){0x00, 0x02, 0x00}, 3, 0},
    {0xE5, (uint8_t[]){0x01, 0x02, 0x00}, 3, 0},
    {0xDE, (uint8_t[]){0x00}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x3A, (uint8_t[]){0x05}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x22, 0x00, 0xCD}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0x3F}, 4, 0},
    {0xDE, (uint8_t[]){0x02}, 1, 0},
    {0xE5, (uint8_t[]){0x00, 0x02, 0x00}, 3, 0},
    {0xDE, (uint8_t[]){0x00}, 1, 0},
    {0x29, (uint8_t []){ 0x00 }, 0, 0},
};

static esp_lcd_panel_io_handle_t s_panel_io = NULL;   /* LCD 面板 IO 句柄 (SPI 读写通道) */
static esp_lcd_panel_handle_t s_panel = NULL;         /* LCD 面板句柄 */
static int64_t s_slpo_us = 0;                         /* SLPOUT 命令发出的时刻 (us), 用于计时 */

#define LCD_SLEEP_OUT_MS  120   /* 退出休眠后需等待的稳定时间 */

/* 前半段 (app_main 头部): 建 SPI/IO/面板 + 发 SLPOUT (记时刻).
 * 硬件复位由 bootloader 完成, 不再调用 esp_lcd_panel_reset.
 * 非阻塞: SLPOUT 后的 120ms 由启动/UI 构建时间自然覆盖, 后半段检查补齐.
 * host: SPI 外设编号 (SPI2_HOST/SPI3_HOST). 返回面板句柄. */
esp_lcd_panel_handle_t lcd_init_early(spi_host_device_t host)
{
    /* SPI bus: 配置 SCLK/MOSI, 其余脚不用则 -1 */
    spi_bus_config_t buscfg = {
        .sclk_io_num     = PIN_LCD_SCLK,
        .mosi_io_num     = PIN_LCD_MOSI,
        .miso_io_num     = -1,                                   /* LCD 只写, 不接 MISO */
        .quadwp_io_num   = -1,                                   /* 不用 QSPI 的 WP 脚 */
        .quadhd_io_num   = -1,                                   /* 不用 QSPI 的 HD 脚 */
        .max_transfer_sz = LCD_H_RES * LCD_V_RES * 2,            /* 最大传输: 整屏 16bit 像素 */
    };
    ESP_ERROR_CHECK(spi_bus_initialize(host, &buscfg, SPI_DMA_CH_AUTO));   /* 自动分配 DMA 通道 */

    /* Panel IO: 定义 SPI 协议层的命令/数据线时序 */
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num       = PIN_LCD_CS,
        .dc_gpio_num       = PIN_LCD_DC,
        .spi_mode          = 0,                  /* SPI mode 0 (CPOL=0, CPHA=0) */
        .pclk_hz           = 80 * 1000 * 1000,   /* SPI 时钟 80MHz */
        .trans_queue_depth = 1,                  /* 事务队列深度 */
        .lcd_cmd_bits      = 8,                  /* 命令字长 8bit */
        .lcd_param_bits    = 8,                  /* 参数字长 8bit */
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)host,
                                              &io_config, &s_panel_io));

    /* Panel: 绑定 JD9853 驱动, 附带寄存器初始化序列 */
    jd9853_vendor_config_t vendor_cfg = {
        .init_cmds      = init_cmds,
        .init_cmds_size = sizeof(init_cmds) / sizeof(jd9853_lcd_init_cmd_t),
    };
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_LCD_RST,          /* -1: 驱动不接管复位脚 (脉冲由 bootloader 在 GPIO25 完成) */
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,   /* 像素通道顺序 RGB */
        .bits_per_pixel = 16,                   /* RGB565 */
        .vendor_config  = &vendor_cfg,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_jd9853(s_panel_io, &panel_config, &s_panel));

    /* 兜底: 复位脉冲已由 bootloader 钩子在 GPIO25 上完成并保持高;
     * 这里再显式把该脚配为输出高, 防止复位线悬空/被意外拉低导致面板不亮. */
    {
        gpio_config_t rst_cfg = {
            .mode         = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << PIN_LCD_RST_HW,
        };
        gpio_config(&rst_cfg);
        gpio_set_level(PIN_LCD_RST_HW, 1);
    }

    /* SLPOUT: 退出休眠 (bootloader 复位后默认进休眠). 记录发送时刻. */
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(s_panel_io, 0x11, NULL, 0));
    s_slpo_us = esp_timer_get_time();

    /* 背光 (初始占空比 0, 开机渐入接管) */
    lcd_backlight_init();

    ESP_LOGI(TAG, "LCD early done (SLPOUT t=%lld us)", s_slpo_us);
    return s_panel;
}

/* 后半段 (lvgl 任务, 无限循环前): 距 SLPOUT ≥120ms 后发寄存器命令 + DISPON */
esp_lcd_panel_handle_t lcd_init_finish(void)
{
    /* 若前半段流逝时间不足 120ms, 这里补足 */
    int64_t remain_us = LCD_SLEEP_OUT_MS * 1000 - (esp_timer_get_time() - s_slpo_us);
    if (remain_us > 0) {
        vTaskDelay(pdMS_TO_TICKS((remain_us + 999) / 1000));
    }

    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));              /* 执行寄存器初始化序列 */
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true)); /* DISPON 点亮 */
    esp_lcd_panel_set_gap(s_panel, 34, 0);                     /* 面板横向起始偏移 34 (驱动 IC 列寻址偏移) */
    esp_lcd_panel_mirror(s_panel, true, true);                 /* 水平+垂直镜像, 匹配安装方向 */
    esp_lcd_panel_invert_color(s_panel, true);                 /* 颜色反相 (该面板需要) */

    ESP_LOGI(TAG, "LCD init done (%dx%d), gap=(34,0)", LCD_H_RES, LCD_V_RES);
    return s_panel;
}

/* 面板句柄访问 (LVGL 注册显示驱动用, 早于 lcd_init_finish) */
esp_lcd_panel_handle_t lcd_get_panel(void)
{
    return s_panel;
}

/* 同步刷屏: 先排入 DMA 后台传输, 再通过一次无命令 tx_param 等待在途 color 事务全部排空,
 * 确保返回时 color_data 不再被 DMA 读取 (esp_lcd_panel_io_tx_color 为异步,
 * 调用方须等 on_color_trans_done/tx_param 排空后才能复用或释放缓冲). */
esp_err_t lcd_draw_bitmap_sync(int x_start, int y_start, int x_end, int y_end,
                               const void *color_data)
{
    ESP_RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(s_panel, x_start, y_start, x_end, y_end, color_data),
                        TAG, "draw bitmap failed");
    /* 无命令/无参数的 tx_param: 语义上是"等队列清空再执行", 充当 DMA 屏障 */
    return esp_lcd_panel_io_tx_param(s_panel_io, -1, NULL, 0);
}

/* ────────────────────────── Backlight (LEDC PWM) ────────────────────────── */
#define BL_PWM_FREQ_HZ  5000   /* 背光 PWM 频率 5kHz (高于人耳可闻, 避免电感啸叫) */

/* 亮度感知曲线 (γ=2.2 对数映射): 滑块值 0~255 → LEDC 占空比.
 * 人眼感知近似对数, 线性占空比在低亮度段变化过快, 查表补偿.
 * duty = max(1, 255 * (v/255)^2.2): 最低保底 1, 避免滑块拖到底屏幕全黑 */
static const uint8_t s_brightness_lut[256] = {
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2,
    3, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 6,
    6, 7, 7, 7, 8, 8, 8, 9, 9, 9, 10, 10, 11, 11, 11, 12,
    12, 13, 13, 13, 14, 14, 15, 15, 16, 16, 17, 17, 18, 18, 19, 19,
    20, 20, 21, 22, 22, 23, 23, 24, 25, 25, 26, 26, 27, 28, 28, 29,
    30, 30, 31, 32, 33, 33, 34, 35, 35, 36, 37, 38, 39, 39, 40, 41,
    42, 43, 43, 44, 45, 46, 47, 48, 49, 49, 50, 51, 52, 53, 54, 55,
    56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71,
    73, 74, 75, 76, 77, 78, 79, 81, 82, 83, 84, 85, 87, 88, 89, 90,
    91, 93, 94, 95, 97, 98, 99, 100, 102, 103, 105, 106, 107, 109, 110, 111,
    113, 114, 116, 117, 119, 120, 121, 123, 124, 126, 127, 129, 130, 132, 133, 135,
    137, 138, 140, 141, 143, 145, 146, 148, 149, 151, 153, 154, 156, 158, 159, 161,
    163, 165, 166, 168, 170, 172, 173, 175, 177, 179, 181, 182, 184, 186, 188, 190,
    192, 194, 196, 197, 199, 201, 203, 205, 207, 209, 211, 213, 215, 217, 219, 221,
    223, 225, 227, 229, 231, 234, 236, 238, 240, 242, 244, 246, 248, 251, 253, 255,
};

/* 背光 PWM 初始化: 配置 LEDC 定时器 + 通道, 初始占空比 0 (不开屏) */
static void lcd_backlight_init(void)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,   /* 低速模式 (ESP32 上 CH0~7) */
        .duty_resolution = LEDC_TIMER_8_BIT,      /* 8bit 分辨率, 占空比 0~255 */
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = BL_PWM_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,         /* 自动选时钟源 */
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    ledc_channel_config_t ch_cfg = {
        .gpio_num   = PIN_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .intr_type  = LEDC_INTR_DISABLE,          /* 不需要中断 */
        .timer_sel  = LEDC_TIMER_0,
        .duty       = 0,                          /* 初始全灭 */
        .hpoint     = 0,                          /* PWM 相位起点 */
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch_cfg));
}

/* 设定背光亮度: level=亮度值 0~255, 经 γ 曲线查表后写 LEDC 占空比 */
void lcd_set_brightness(uint8_t level)
{
    uint8_t duty = s_brightness_lut[level];       /* 感知线性化映射 */
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);   /* 更新目标占空比 */
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);      /* 使占空比实际生效 */
}
