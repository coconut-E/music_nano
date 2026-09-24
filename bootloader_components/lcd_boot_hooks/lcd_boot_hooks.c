#include "esp_log.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "soc/soc.h"
#include "soc/gpio_reg.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/rtc_io_reg.h"

/* 该组件编译进 bootloader, 在 bootloader 阶段提前完成 LCD 硬件上电时序,
 * 避免等 app 起来后再花 120ms 等 LCD 稳定, 从而缩短开机黑屏时间. */

#define PIN_PERI_PWR   22   /* 外设供电 GPIO: 输出低电平 = 供电开启 (低有效) */
#define PIN_LCD_RST    25   /* LCD 复位 GPIO: 低有效, 拉低复位/拉高释放 */

/* 空函数: 供链接器引用, 防止整个组件被静态链接优化裁掉 */
void bootloader_hooks_include(void) {}

/* bootloader 初始化完成后的钩子: 释放深睡保持 -> 上电 -> 复位 LCD */
void bootloader_after_init(void)
{
    /* 0) 释放深睡期间的引脚保持.
     * app 深睡前对 GPIO22 调用了 gpio_hold_en + gpio_deep_sleep_hold_en,
     * 二者都落在 RTC 域并跨唤醒复位保留; 不清掉的话下面"拉低 GPIO22 上电"不生效,
     * 复位脉冲会打在断电的外设上 (电池唤醒黑屏的根因之一). */
    CLEAR_PERI_REG_MASK(RTC_CNTL_DIG_ISO_REG, RTC_CNTL_DG_PAD_AUTOHOLD_EN_M);
    REG_WRITE(RTC_IO_DIG_PAD_HOLD_REG, 0);

    /* 1) 打开外设电源 (PIN_PERI_PWR=22): 拉低输出 */
    esp_rom_gpio_pad_select_gpio(PIN_PERI_PWR);                    /* 把该引脚配置为 GPIO 功能 */
    REG_SET_BIT(GPIO_ENABLE_REG, BIT(PIN_PERI_PWR));               /* GPIO 方向置为输出 (enable 寄存器) */
    REG_SET_BIT(GPIO_OUT_W1TC_REG, BIT(PIN_PERI_PWR));            /* 输出置低 (W1TC = write-1-to-clear), 上电 */
    esp_rom_delay_us(10 * 1000);                                   /* 等电源稳定 10ms */

    /* 2) 复位 LCD: 先拉低 (复位有效), 保持 10ms 后拉高 (释放复位), 结束时保持输出高.
     * 引脚号 <32 用 GPIO_ENABLE/OUT 寄存器, >=32 用 *_1_REG + BIT(n-32). */
    esp_rom_gpio_pad_select_gpio(PIN_LCD_RST);                     /* 配置为 GPIO 功能 */
#if PIN_LCD_RST < 32
    REG_SET_BIT(GPIO_ENABLE_REG, BIT(PIN_LCD_RST));                /* 置为输出 */
    REG_SET_BIT(GPIO_OUT_W1TC_REG, BIT(PIN_LCD_RST));              /* 拉低 = 进入复位 */
    esp_rom_delay_us(10 * 1000);                                   /* 复位保持 10ms */
    REG_SET_BIT(GPIO_OUT_W1TS_REG, BIT(PIN_LCD_RST));              /* 拉高 = 释放复位 */
#else
    REG_SET_BIT(GPIO_ENABLE1_REG, BIT(PIN_LCD_RST - 32));          /* 引脚号≥32 用 EN1 寄存器, 置输出 */
    REG_SET_BIT(GPIO_OUT1_W1TC_REG, BIT(PIN_LCD_RST - 32));        /* 拉低 = 进入复位 */
    esp_rom_delay_us(10 * 1000);                                   /* 复位保持 10ms */
    REG_SET_BIT(GPIO_OUT1_W1TS_REG, BIT(PIN_LCD_RST - 32));        /* 拉高 (W1TS = write-1-to-set) = 释放复位 */
#endif
    ESP_LOGI("HOOK", "RST released (GPIO%d)", PIN_LCD_RST);
}
