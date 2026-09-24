#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "touch.h"

#define TAG "touch"

/* 触摸芯片 (CST816) I2C 配置 */
#define TOUCH_I2C_PORT      I2C_NUM_0   /* 使用的 I2C 外设 */
#define TOUCH_SCL_IO        27          /* SCL 引脚 */
#define TOUCH_SDA_IO        26          /* SDA 引脚 */
#define TOUCH_ADDR          0x15        /* CST816 7bit I2C 地址 */

#define TOUCH_INT_IO        33          /* INT 引脚 (低脉冲, 下降沿触发) */
#define TOUCH_CMD_TIMEOUT_MS 5          /* 短超时: 事务卡住也快速返回, 不拖累 LVGL */

static volatile bool s_int_flag = false;   /* INT 下降沿标志 (ISR 置位, 任务清) */
static bool          s_present  = false;   /* 当前是否有有效触点 (仅任务写) */
static int32_t       s_x        = 0;       /* 最近一次原始 X 坐标 */
static int32_t       s_y        = 0;       /* 最近一次原始 Y 坐标 */

/* GPIO INT ISR: 只锁存标志. INT 脉冲内有毛刺会多次触发, 但反复置位同一 bool 无副作用 */
static void IRAM_ATTR touch_int_isr(void *arg)
{
    (void)arg;
    s_int_flag = true;
}

/* 完整读取: 成功返回 true 并给出原始坐标 (换算/钳制由 LVGL 层做).
 * 寄存器 0x01 起读 7 字节: [手势, 手指数, x高, x低, y高, y低, 压力]. */
static bool touch_read_raw(int32_t *x, int32_t *y)
{
    uint8_t reg = 0x01;
    uint8_t buf[7] = {0};

    esp_err_t ret = i2c_master_write_read_device(TOUCH_I2C_PORT, TOUCH_ADDR,
        &reg, 1, buf, sizeof(buf), pdMS_TO_TICKS(TOUCH_CMD_TIMEOUT_MS));
    if (ret != ESP_OK || buf[1] == 0) return false;   /* 读失败或手指数=0 → 无触点 */

    /* 坐标按 CST816 格式拼接: 12bit, 高 4 位在第二字节低半字节 */
    *x = ((buf[2] & 0x0F) << 8) | buf[3];
    *y = ((buf[4] & 0x0F) << 8) | buf[5];
    return true;
}

/* I2C 总线初始化: 400kHz 主模式, 使能内部上拉 */
static void touch_bus_init(void)
{
    i2c_config_t conf = {
        .mode          = I2C_MODE_MASTER,
        .sda_io_num    = TOUCH_SDA_IO,
        .scl_io_num    = TOUCH_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,          /* 400k 标准快速模式 */
    };
    ESP_ERROR_CHECK(i2c_param_config(TOUCH_I2C_PORT, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(TOUCH_I2C_PORT, conf.mode, 0, 0, 0));   /* 不配接收/发送缓冲 */
}

/* 初始化: I2C 总线 + GPIO33 输入/上拉/下降沿中断 */
void touch_init(void)
{
    touch_bus_init();

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << TOUCH_INT_IO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,     /* INT 为低脉冲, 空闲需上拉保持高 */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,      /* 下降沿触发 */
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {   /* 已装过则忽略 */
        ESP_LOGE(TAG, "GPIO ISR 服务安装失败: %s", esp_err_to_name(err));
    }
    ESP_ERROR_CHECK(gpio_isr_handler_add(TOUCH_INT_IO, touch_int_isr, NULL));

    s_int_flag = false;
    s_present  = false;
    ESP_LOGI(TAG, "触摸就绪 (INT=GPIO%d)", TOUCH_INT_IO);
}

/* 读触点: 有中断 或 当前判定按下时才读 I2C; 空闲完全不读.
 * 按下期间芯片清醒, 读总线安全; 松开后某次读返回无触点即清 present. */
bool touch_poll(int32_t *x, int32_t *y)
{
    if (s_int_flag || s_present) {
        s_int_flag = false;   /* 先清标志: 读取期间新中断可再次置位 */
        int32_t nx, ny;
        if (touch_read_raw(&nx, &ny)) {
            s_x = nx;
            s_y = ny;
            s_present = true;
        } else {
            s_present = false;
        }
    }

    if (s_present) {
        *x = s_x;
        *y = s_y;
    }
    return s_present;
}

/* 清触点/中断状态 (息屏时调用, 防止残留触点卡住界面) */
void touch_reset(void)
{
    s_int_flag = false;
    s_present  = false;
}
