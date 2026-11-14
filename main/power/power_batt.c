/* 电池电压: ADC 采样、开机低压提示、运行中低压关机.
 * 从 power_mgr.c 拆出来的原因: 它和"什么时候睡、怎么睡"没有关系, 只是共用
 * "立刻断电深睡"这一个动作 (power_mgr_shutdown_now). */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "drv_display.h"
#include "sys_monitor.h"
#include "atomic_utils.h"
#include "battery_low_img.h"
#include "power_mgr.h"
#include "power_batt.h"

static const char *TAG = "BATT";

/* ── 电池电压 ADC (开机检测 + sys_monitor 周期采样共用) ── */
#define VBAT_ADC_UNIT   ADC_UNIT_1    /* 电池电压使用的 ADC 单元 */
#define VBAT_ADC_CHAN   ADC_CHANNEL_7 /* 电池电压 ADC 通道 */
#define VBAT_DIVIDER_RATIO 2.0f       /* 分压比: 电阻分压 2:1, 电压乘 2 还原 */

#define VBAT_BOOT_SAMPLES  3          /* 开机检测采样次数 (取平均滤波) */
#define VBAT_SAMPLE_DELAY_MS 5        /* 相邻两次采样间隔 */

#define VBAT_BOOT_LOW_V   3.35f       /* 开机低电量阈值 (V) */
#define VBAT_LOW_BRI      128         /* 低电量提示亮度 */
#define VBAT_LOW_HOLD_MS  2000        /* 低电量提示停留时间 */
#define VBAT_BAND_ROWS    32          /* 分块写屏行数 (172x32 → 10 块刷完一屏, 避免整屏大事务行错位) */

static adc_oneshot_unit_handle_t s_adc_handle = NULL;   /* ADC 单元句柄 */
static adc_cali_handle_t         s_cali_handle = NULL;  /* ADC 校准句柄 (eFuse 两点校准) */
static bool                      s_adc_inited = false;  /* ADC 已初始化标志 (幂等) */

/* 初始化 ADC 单元/通道/校准 (幂等: 已初始化则跳过).
 * 返回 true=可用, false=初始化失败 (调用方应跳过低压判断, 避免误判 0V 深睡) */
static bool vbat_adc_init(void)
{
    if (s_adc_inited) return s_adc_handle != NULL;
    s_adc_inited = true;

    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = VBAT_ADC_UNIT,
    };
    if (adc_oneshot_new_unit(&unit_cfg, &s_adc_handle) != ESP_OK) {
        ESP_LOGE(TAG, "vbat ADC 单元初始化失败");
        s_adc_handle = NULL;
        return false;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_oneshot_config_channel(s_adc_handle, VBAT_ADC_CHAN, &chan_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "vbat ADC 通道配置失败");
        adc_oneshot_del_unit(s_adc_handle);
        s_adc_handle = NULL;
        return false;
    }

    adc_cali_line_fitting_config_t cali_cfg = {
        .unit_id  = VBAT_ADC_UNIT,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_cali_create_scheme_line_fitting(&cali_cfg, &s_cali_handle) != ESP_OK) {
        ESP_LOGW(TAG, "eFuse 两点校准不可用, 退回线性估算");
        s_cali_handle = NULL;   /* 仍可用线性兜底 */
    }
    return true;
}

/* 单次读取电池电压: ADC 原始值 → mV (优先 eFuse 校准, 否则线性估算) → V × 分压比.
 * 返回电压(V); 返回负值表示 ADC 不可用/读取失败 (调用方不得据此判定低压) */
float power_batt_read_once(void)
{
    if (!vbat_adc_init()) return -1.0f;

    int raw = 0;
    if (adc_oneshot_read(s_adc_handle, VBAT_ADC_CHAN, &raw) != ESP_OK) {
        return -1.0f;
    }

    int mv = 0;
    if (s_cali_handle != NULL) {
        adc_cali_raw_to_voltage(s_cali_handle, raw, &mv);   /* 校准曲线换算 */
    } else {
        mv = raw * 3300 / 4095;   /* 兜底线性: 3.3V/12bit */
    }
    return (float)mv / 1000.0f * VBAT_DIVIDER_RATIO;   /* mV→V, 再乘分压比还原真实电压 */
}

/* 开机低压检测: 采样平均 ≥ 阈值直接返回 (正常启动);
 * < 阈值: 补齐 LCD 后半段 → 整屏写低电图标 → 亮度 128 停留 2 秒 → 深睡 (不返回) */
void power_batt_boot_check(void)
{
    if (!vbat_adc_init()) {
        ESP_LOGW(TAG, "电池 ADC 不可用, 跳过开机低压检测 (按正常启动)");
        return;   /* 不得据 0V 判定低电, 否则会误深睡 */
    }

    float sum = 0.0f;
    int   valid = 0;
    for (int i = 0; i < VBAT_BOOT_SAMPLES; i++) {
        float v = power_batt_read_once();
        if (v >= 0.0f) { sum += v; valid++; }
        vTaskDelay(pdMS_TO_TICKS(VBAT_SAMPLE_DELAY_MS));
    }
    if (valid == 0) {
        ESP_LOGW(TAG, "电池电压采样全部失败, 跳过开机低压检测 (按正常启动)");
        return;
    }
    float avg = sum / (float)valid;

    atomic_store_float(&g_vbat, avg);   /* 写入全局变量, 供 UI/console 读取 */
    ESP_LOGI(TAG, "开机电池电压: %.2f V", (double)avg);

    if (avg >= VBAT_BOOT_LOW_V) {
        return;   /* 正常启动, 由后续初始化继续 (lcd_init_finish 在 LVGL 任务里做) */
    }

    ESP_LOGW(TAG, "电池电压 %.2f V < %.2f V, 低电量提示后深睡", (double)avg, (double)VBAT_BOOT_LOW_V);

    /* 完成 LCD 后半段初始化 (补齐 SLPOUT≥120ms + DISPON) */
    lcd_init_finish();

    /* 整屏写入低电量图: 申请 172x32 行带缓冲, 分 10 块刷完一屏.
     * 用 lcd_draw_bitmap_sync (含 DMA 屏障) 每块等传输完成后再进下一块,
     * 避免 esp_lcd 异步 DMA 仍在读缓冲就被改写/释放而导致的错位/抽搐. */
    const size_t band_bytes = (size_t)BATTERY_LOW_IMG_W * VBAT_BAND_ROWS * 2;
    uint8_t *buf = heap_caps_malloc(band_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

    if (buf) {
        for (int y = 0; y < BATTERY_LOW_IMG_H; y += VBAT_BAND_ROWS) {
            int rows = VBAT_BAND_ROWS;
            if (y + rows > BATTERY_LOW_IMG_H) rows = BATTERY_LOW_IMG_H - y;
            size_t copy = (size_t)BATTERY_LOW_IMG_W * rows * 2;
            memcpy(buf, battery_low_img + (size_t)y * BATTERY_LOW_IMG_W * 2, copy);
            if (lcd_draw_bitmap_sync(0, y, BATTERY_LOW_IMG_W, y + rows, buf) != ESP_OK) {
                ESP_LOGE(TAG, "低电量图写屏失败 (y=%d)", y);
            }
        }
        heap_caps_free(buf);
    } else {
        ESP_LOGE(TAG, "低电量图缓冲分配失败");
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    lcd_set_brightness(VBAT_LOW_BRI);   /* 刷屏完成后拉高背光 */
    vTaskDelay(pdMS_TO_TICKS(VBAT_LOW_HOLD_MS));   /* 停留 2 秒 */

    lcd_set_brightness(0);              /* 拉低背光 */
    vTaskDelay(pdMS_TO_TICKS(50));      /* 留出背光熄灭时间 */
    power_mgr_shutdown_now();           /* 进入深度睡眠 (不返回) */
}

/* 运行中电压过低: 拉低背光 → 切外设供电 → 深睡 (不返回).
 * 调用场景: sys_monitor 周期采样发现 g_vbat < 临界值 */
void power_batt_critical_shutdown(void)
{
    ESP_LOGW(TAG, "电压过低, 紧急关机进入深度睡眠");
    lcd_set_brightness(0);              /* 直接拉低背光 (无淡出) */
    vTaskDelay(pdMS_TO_TICKS(20));      /* 留出背光熄灭时间 */
    power_mgr_shutdown_now();           /* 切外设供电 + 唤醒 + 深睡 (不返回) */
}
