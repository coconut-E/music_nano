#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_heap_caps.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_rom_uart.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"
#include "drv_display.h"
#include "settings.h"
#include "bt_a2dp.h"
#include "autosleep.h"
#include "ui_core.h"
#include "touch.h"
#include "mode_anim.h"
#include "sys_monitor.h"
#include "app_mode.h"
#include "ui_player.h"
#include "ui_settings.h"
#include "battery_low_img.h"
#include "atomic_utils.h"
#include "power_mgr.h"

static const char *TAG = "POWER";

/* GPIO37: 息屏/唤醒按键 (外部 10k 下拉, 仅输入, 只测上升沿) */
#define PIN_PWR_KEY      GPIO_NUM_37
/* GPIO22: 外设供电控制 (高=切断外设供电), 深睡时保持 */
#define PIN_PWR_CUT      GPIO_NUM_22

/* ── 电池电压 ADC (开机检测 + sys_monitor 周期采样共用) ── */
#define VBAT_ADC_UNIT   ADC_UNIT_1    /* 电池电压使用的 ADC 单元 */
#define VBAT_ADC_CHAN   ADC_CHANNEL_7 /* 电池电压 ADC 通道 */
#define VBAT_DIVIDER_RATIO 2.0f       /* 分压比: 电阻分压 2:1, 电压乘 2 还原 */

#define VBAT_BOOT_SAMPLES  3          /* 开机检测采样次数 (取平均滤波) */
#define VBAT_SAMPLE_DELAY_MS 5       /* 相邻两次采样间隔 */

#define VBAT_BOOT_LOW_V   3.35f       /* 开机低电量阈值 (V) */
#define VBAT_LOW_BRI      128         /* 低电量提示亮度 */
#define VBAT_LOW_HOLD_MS  2000        /* 低电量提示停留时间 */
#define VBAT_BAND_ROWS    32          /* 分块写屏行数 (172x32 → 10 块刷完一屏, 避免整屏大事务行错位) */

static adc_oneshot_unit_handle_t s_adc_handle = NULL;   /* ADC 单元句柄 */
static adc_cali_handle_t         s_cali_handle = NULL;  /* ADC 校准句柄 (eFuse 两点校准) */
static bool                      s_adc_inited = false;  /* ADC 已初始化标志 (幂等) */

/* 息屏时 LVGL 刷新周期: 100 秒一次 (省电, 屏幕已黑无需频繁刷新) */
#define LVGL_SCREEN_OFF_REFRESH_MS  100000

/* 淡出/淡入: 变化幅度 1~255 → 总时长 50~400ms (线性映射, 避免拉满后过长) */
#define BRI_FADE_TIME_MIN_MS  50
#define BRI_FADE_TIME_MAX_MS  400

/* 设置页进出: 背光淡出/淡入的时长. 和光圈动画的 150/180ms 对齐,
 * 否则按亮度差映射会拉到 400ms 一程, 满亮度下整个切换 0.8s 太长 */
#define MODE_FADE_OUT_MS      150
#define MODE_FADE_IN_MS       180

/* 按键冷却: 吞机械抖动产生的二次上升沿 */
#define PWR_KEY_COOLDOWN_MS  200
/* 长按判定: 按住超过此值进入主菜单 */
#define PWR_KEY_LONG_US      (800 * 1000)

/* 无操作自动熄屏: "通用 + 特殊"两层次, 单位分钟 (1~10, 设置页可改).
 *   通用 = 菜单 / 设置 / 音乐封面, 以及以后新加的任何模式;
 *   特殊 = 小说、音乐歌词页 (各自覆盖通用).
 * 超时后执行与睡眠键短按相同的动作: BT 已连 → 只熄屏; 未连 → 淡出深睡.
 * 菜单/设置里蓝牙任务已被 bt_a2dp_pause 挂起 (状态=断开), 所以到点是"淡出后直接深睡".
 * 这里缓存一份 (1 秒轮询每次都读, 不能读 NVS), 开机由 power_mgr_init 灌入. */
static int s_idle_min[SETTINGS_IDLE_NUM] = { 2, 5, 5 };   /* 与 settings.c 的默认值一致 */

#define PWR_IDLE_POLL_MS     1000

/* 电源状态机 */
typedef enum {
    PWR_ACTIVE,       /* 正常: 亮屏 */
    PWR_SCREEN_OFF,   /* 手动息屏 (BT 保持连接, 音频继续) */
    PWR_FAKE_OFF,     /* 淡出中(即将深睡), 忽略按键 */
    PWR_LIGHT_SLEEP,  /* 淡出/浅睡中 (5 分钟后转深睡), 忽略按键 */
} pwr_state_t;

/* 睡眠抑制: OTA 新固件校验窗口期间置位.
 * 窗口内电源键短按/长按、无操作熄屏全部被拦下 —— 用户只会看到遮罩上的
 * "请勿断电", 按什么都没反应, 避免误触导致新固件来不及打标就被回退. */
static volatile bool s_sleep_inhibit = false;

/* 界面抑制: 设置页的 WiFi 流程在扫描/连接/释放期间置位.
 * 和 s_sleep_inhibit 分开存、互不覆盖 (OTA 的校验窗口可能同时也在用),
 * 两者任一为真 → 电源键短按/长按与无操作熄屏全部失效.
 * 正好就是需求里"扫描/连接中按电源键、按退出都无反应"的语义, 由 UI 按阶段开关. */
static volatile bool s_ui_inhibit = false;

static pwr_state_t s_state = PWR_ACTIVE;   /* 当前电源状态 */
static bool        s_key_prev = false;     /* 上次按键电平 (用于检测沿) */
static int64_t     s_key_last_us = 0;      /* 上次按键沿的时刻 (us), 用于冷却 */
static int64_t     s_key_press_us = 0;     /* 本次按下的时刻 (us), 长按判定用 */
static bool        s_key_pending = false;  /* 已按下且尚未判定 (短按待松手触发) */
static bool        s_key_long_fired = false; /* 本次长按已处理 */
static uint8_t     s_cur_bri = 0;          /* 当前实际背光亮度 */
static int         s_fade_dummy = 0;       /* 动画变量占位 (值由 bri_fade_exec 使用) */
static int64_t     s_last_activity_us = 0; /* 最近一次用户操作时刻 (us), 无操作熄屏用 */
static bool        s_lyrics_active = false; /* 音乐模式下当前是否显示歌词页 (3 分钟超时) */

/* 模式切换状态 */
static bool  s_mode_switching = false;     /* 切换过渡中: 屏蔽按键/蓝牙轮询 */
static void (*s_switch_work)(void) = NULL; /* 切换时黑屏窗口内执行的工作 */

/* 淡出/淡入执行: 直接写 LEDC 背光.
 * var=动画变量地址(未用), v=动画当前值 (目标亮度) */
static void bri_fade_exec(void *var, int32_t v)
{
    (void)var;
    s_cur_bri = (uint8_t)v;
    lcd_set_brightness(s_cur_bri);
}

/* 外部亮度变化 (亮度滑块) 时同步当前亮度, 保证下次淡入/淡出起点正确.
 * v=新的当前亮度 */
void power_mgr_set_cur_bri(uint8_t v)
{
    s_cur_bri = v;
}

/* 记录一次用户操作: 触摸层每次读到触点、按键按下/步进时调用.
 * 所有调用方都在 LVGL 任务上下文, 与 idle 定时器同任务, 无需加锁. */
void power_mgr_notify_activity(void)
{
    s_last_activity_us = esp_timer_get_time();
}

/* 睡眠抑制开关 (OTA 校验窗口用): inhibit=true 时三个睡眠入口全部失效.
 * 只拦"睡眠/熄屏"这类主动动作, 触摸/音量键不受影响. */
void power_mgr_set_sleep_inhibit(bool inhibit)
{
    if (s_sleep_inhibit == inhibit) return;
    s_sleep_inhibit = inhibit;
    ESP_LOGI(TAG, "睡眠抑制 %s", inhibit ? "开启" : "解除");
    if (!inhibit) power_mgr_notify_activity();   /* 解除时重新计时, 避免立刻熄屏 */
}

bool power_mgr_sleep_inhibited(void)
{
    return s_sleep_inhibit || s_ui_inhibit;
}

/* 界面抑制开关 (设置页 WiFi 流程用): 只影响"按键 + 无操作熄屏",
 * 与 OTA 的 sleep_inhibit 各存一个标志, 谁也不会把对方清掉. */
void power_mgr_set_ui_inhibit(bool inhibit)
{
    if (s_ui_inhibit == inhibit) return;
    s_ui_inhibit = inhibit;
    ESP_LOGI(TAG, "界面抑制(按键/熄屏) %s", inhibit ? "开启" : "解除");
    if (!inhibit) power_mgr_notify_activity();   /* 解除时重新计时, 避免立刻熄屏 */
}

/* 歌词页显示状态: true=歌词页 (3 分钟), false=封面 (1 分钟).
 * 由 UI 在进入/退出歌词页及销毁音乐组时调用, 与 idle 定时器同任务. */
void power_mgr_set_lyrics_active(bool active)
{
    if (s_lyrics_active == active) return;
    s_lyrics_active = active;
    power_mgr_notify_activity();   /* 模式变化后重新计时 */
}

/* 淡入/淡出时长: 变化幅度 diff → 50~400ms 线性映射 (diff=0 不淡).
 * diff=亮度变化量 (0~255) */
static int bri_fade_time_ms(int diff)
{
    if (diff <= 0) return 1;
    if (diff > 255) diff = 255;
    return BRI_FADE_TIME_MIN_MS + (diff - 1) * (BRI_FADE_TIME_MAX_MS - BRI_FADE_TIME_MIN_MS) / (255 - 1);
}

/* LVGL 刷新频率: slow=true 降为 100s 一次, false 恢复 20ms */
static void set_lvgl_slow_refresh(bool slow)
{
    lv_timer_t *refr = _lv_disp_get_refr_timer(lv_disp_get_default());   /* 拿 LVGL 刷新定时器 */
    if (!refr) return;
    if (slow) {
        lv_timer_set_period(refr, LVGL_SCREEN_OFF_REFRESH_MS);   /* 息屏: 刷新周期拉长到 100s */
    } else {
        lv_timer_set_period(refr, LV_DISP_DEF_REFR_PERIOD);      /* 亮屏: 恢复默认周期 */
        lv_timer_resume(refr);                                   /* 确保定时器处于运行态 */
    }
}

/* 淡到目标亮度: 时长 = 变化幅度映射到 50~400ms; done 为动画完成回调.
 * target=目标亮度, done=完成后回调 (如 enter_deep_sleep) */
static void screen_fade(uint8_t target, lv_anim_ready_cb_t done)
{
    int diff = (int)s_cur_bri - (int)target;   /* 变化幅度 */
    if (diff < 0) diff = -diff;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &s_fade_dummy);                       /* 动画对象 */
    lv_anim_set_exec_cb(&a, bri_fade_exec);                   /* 每帧执行: 写背光 */
    lv_anim_set_values(&a, s_cur_bri, target);                /* 起始→目标亮度 */
    lv_anim_set_time(&a, bri_fade_time_ms(diff));             /* 时长按幅度映射 */
    lv_anim_set_path_cb(&a, lv_anim_path_linear);             /* 线性插值 */
    if (done) lv_anim_set_ready_cb(&a, done);                 /* 完成回调 */
    lv_anim_start(&a);
}

/* ── 细步长背光淡化 (只给模式过渡用) ──
 *
 * 不用 lv_anim: 它的定时器是 LV_DISP_DEF_REFR_PERIOD(20ms) 一跳, 150ms 的过渡只有
 * 7 步 —— 白底页面上每步约 13% 亮度, 看上去就是一档一档往上跳("闪")。
 * 这里自己开 6ms 的定时器, 同样的总时长走 25 步, 每步 ~4%, 才是连续变暗。
 * 步长只由"驱动频率 / 总时长"决定, 和亮度区间怎么切没关系 —— 所以把量程分一半
 * 给软件淡化并不能让步长变短, 只会把分带接缝(那根灰线)重新引进来。 */
#define BRI_STEP_MS  6

static lv_timer_t *s_bri_timer = NULL;      /* 淡化驱动定时器 */
static int         s_bri_from  = 0;         /* 起始亮度 */
static int         s_bri_to    = 0;         /* 目标亮度 */
static uint32_t    s_bri_ms    = 1;         /* 总时长 */
static int64_t     s_bri_t0    = 0;         /* 起始时刻 (us) */
static void      (*s_bri_done)(void) = NULL;/* 完成回调 */

static void bri_step_cb(lv_timer_t *tmr)
{
    int el = (int)((esp_timer_get_time() - s_bri_t0) / 1000);   /* ms */
    int p  = (s_bri_ms > 0) ? (int)((int64_t)el * 1000 / s_bri_ms) : 1000;
    if (p > 1000) p = 1000;

    int v = s_bri_from + (s_bri_to - s_bri_from) * p / 1000;
    if (v != (int)s_cur_bri) {
        s_cur_bri = (uint8_t)v;
        lcd_set_brightness(s_cur_bri);
    }

    if (p < 1000) return;

    lv_timer_del(tmr);
    s_bri_timer = NULL;
    void (*done)(void) = s_bri_done;
    s_bri_done = NULL;
    if (done) done();
}

/* 从当前亮度线性淡到 target; 走完调 done (可为 NULL). 重复调用会顶掉上一次 */
static void bri_fade_start(int target, uint32_t ms, void (*done)(void))
{
    if (s_bri_timer) { lv_timer_del(s_bri_timer); s_bri_timer = NULL; }
    s_bri_from = s_cur_bri;
    s_bri_to   = target;
    s_bri_ms   = ms ? ms : 1;
    s_bri_t0   = esp_timer_get_time();
    s_bri_done = done;
    s_bri_timer = lv_timer_create(bri_step_cb, BRI_STEP_MS, NULL);
}

/* 实际执行深度睡眠: GPIO22 拉高+保持(切断外设供电), GPIO37 高电平唤醒 (不返回) */
static void enter_deep_sleep_now(void)
{
    ESP_LOGI(TAG, "进入深度睡眠 (GPIO22 高电平保持, GPIO37 高电平唤醒)");

    player_save_progress_now();   /* 深睡前补存播放进度 (非音乐模式 no-op) */

    /* 切外设供电: GPIO22 输出高 + 保持电平 (深睡中 GPIO 状态丢失, 靠 hold 保持) */
    gpio_config_t cut_cfg = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << PIN_PWR_CUT,
    };
    gpio_config(&cut_cfg);
    gpio_set_level(PIN_PWR_CUT, 1);        /* 高 = 断电 */

    vTaskDelay(pdMS_TO_TICKS(300));        /* 让外设电源轨放电 */

    /* 固件配置过的其它 IO 全部复位为高阻 (输入, 关内部上下拉).
     * 否则深睡 autohold 会把它们锁在活动电平, 经断电外设的 GND 灌流 (实测 ~3mA).
     * GPIO22 例外: 需维持 PMOS 关断, 保持输出高。 */
    static const int io_pins[] = {
        1, 3,                  /* UART0 TX/RX */
        2, 13, 14, 15,         /* SDMMC D0 / 检测 / CLK / CMD */
        4, 5, 18, 23, 25, 32,  /* LCD 背光 / CS / SCLK / MOSI / RST / DC */
        26, 27, 33,            /* 触摸 SDA / SCL / INT */
    };
    for (size_t i = 0; i < sizeof(io_pins) / sizeof(io_pins[0]); i++) {
        gpio_num_t p = (gpio_num_t)io_pins[i];
        gpio_reset_pin(p);                      /* 断开外设复用 (SPI/I2C/SDMMC/LEDC) */
        gpio_set_pull_mode(p, GPIO_FLOATING);   /* 关掉 reset 默认使能的上拉 */
        gpio_set_direction(p, GPIO_MODE_INPUT); /* OE=0 → 高阻 */
    }

    gpio_hold_en(PIN_PWR_CUT);             /* 锁定 GPIO22 高电平, 深睡期间不掉 */
    gpio_deep_sleep_hold_en();             /* 允许深睡期间保持 (其余脚已是高阻) */

    /* 先清掉所有唤醒源: 浅睡留下的 5 分钟 timer 若不清, 深睡 5 分钟后会自己醒来 */
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_sleep_enable_ext0_wakeup(PIN_PWR_KEY, 1);   /* 只剩 GPIO37 高电平唤醒 */
    esp_deep_sleep_start();                          /* 进入深睡 (此处不返回) */
}

/* LVGL 淡出完成回调: 转进深度睡眠 */
static void enter_deep_sleep(lv_anim_t *a)
{
    (void)a;
    enter_deep_sleep_now();
}

/* ─────────────────────── 浅睡 (light sleep) ───────────────────────
 * 浅睡 = CPU/AHB/APB 停钟, 但 GPIO22 不动、外设一直供电, 程序从断点继续.
 * 短按(蓝牙断开) / 无操作超时 / 息屏期蓝牙断开 → 先浅睡 N 分钟, 期间按电源键
 * 立刻淡入亮屏, N 分钟到点再走真正的深睡 (enter_deep_sleep_now).
 *
 * N 由设置页「浅睡时长」给 (0~10 分钟), 0 = 不浅睡、直接深睡 (老行为).
 * 缓存在 s_light_sleep_min, 睡眠路径上不读 flash. */
static int s_light_sleep_min = LIGHT_SLEEP_MIN_DEFAULT;

static volatile int s_ls_test_sec = 0;   /* console 请求的浅睡秒数 (0=无请求) */

/* 只置请求: 真正的睡眠必须在 LVGL 任务里做 (和深睡同一上下文, 也保证没有别的任务
 * 正卡在 LVGL/FATFS 的锁里), console 任务只写这个 int */
void power_mgr_light_sleep_test(int sec)
{
    if (sec <= 0) return;
    if (sec > 600) sec = 600;            /* 别让一次测试睡过头 */
    s_ls_test_sec = sec;
}

/* ext0 唤醒会把 GPIO37 切成 RTC IO, 浅睡醒来后必须改回普通数字输入 ——
 * 否则 gpio_get_level 读到的一直是 0, 电源键从此"按不动" (深睡走重启, 不会碰到这个问题). */
static void key_gpio_restore(void)
{
    gpio_config_t key_cfg = {
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = 1ULL << PIN_PWR_KEY,
    };
    gpio_config(&key_cfg);
}

/* 睡前把串口吐干净: 一进浅睡 UART 时钟就停了, 还在驱动/FIFO 里没发出去的字节会被
 * 截断 —— 日志里只剩半句"淡出后浅睡"就是这个原因. */
static void uart_flush_before_sleep(void)
{
    fflush(stdout);
    fsync(STDOUT_FILENO);                    /* stdio/VFS 那一层也刷掉 */
    esp_rom_output_tx_wait_idle(0);          /* UART0: 等到 TX FIFO 真的空 */
}

/* Phase 0 自检: 睡前在 PSRAM / 内部 RAM 各写一块图案, 醒来逐项核对.
 * 任何一项挂掉都说明"浅睡后从断点继续"在这台机器上不成立, 先别接线. */static void light_sleep_test_run(int sec)
{
    enum { PS_BYTES = 1024, IN_BYTES = 256 };

    uint8_t *ps = heap_caps_malloc(PS_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *in = heap_caps_malloc(IN_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if (ps) for (int i = 0; i < PS_BYTES; i++) ps[i] = (uint8_t)(i * 31 + 7);
    if (in) for (int i = 0; i < IN_BYTES; i++) in[i] = (uint8_t)(i ^ 0x5A);

    printf("[ls] 请求浅睡 %d 秒 (PSRAM=%p, 内部=%p), 按电源键可提前唤醒\n",
           sec, (void *)ps, (void *)in);
    uart_flush_before_sleep();                             /* 睡前把串口吐干净 */

    const int64_t    t0 = esp_timer_get_time();
    const TickType_t k0 = xTaskGetTickCount();

    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_sleep_enable_ext0_wakeup(PIN_PWR_KEY, 1);          /* 电源键高电平 */
    esp_sleep_enable_timer_wakeup((uint64_t)sec * 1000000ULL);

    esp_err_t err = esp_light_sleep_start();

    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL); /* 别把 timer 留在武器库 */

    /* 醒来时按键多半还按着: 按"已按住"记账, 否则 10ms 轮询会把它当短按,
     * 醒来立刻又睡回去 */
    key_gpio_restore();
    s_key_prev    = (gpio_get_level(PIN_PWR_KEY) == 1);
    s_key_pending = false;

    const int64_t    dt_us = esp_timer_get_time() - t0;
    const TickType_t dt_tk = xTaskGetTickCount() - k0;

    printf("[ls] 醒来: err=%s cause=%d (%s) esp_timer +%d ms  tick +%d ms\n",
           esp_err_to_name(err), (int)cause,
           cause == ESP_SLEEP_WAKEUP_EXT0  ? "按键" :
           cause == ESP_SLEEP_WAKEUP_TIMER ? "定时" : "其它",
           (int)(dt_us / 1000), (int)(dt_tk * portTICK_PERIOD_MS));

    /* 1) PSRAM: 半睡后内容是否保住 (本工程开了 PSRAM half-sleep) */
    if (!ps) {
        printf("[ls] 1 PSRAM: 分配失败 (跳过)\n");
    } else {
        int bad = -1;
        for (int i = 0; i < PS_BYTES; i++) {
            if (ps[i] != (uint8_t)(i * 31 + 7)) { bad = i; break; }
        }
        if (bad < 0) {
            printf("[ls] 1 PSRAM 1KB: OK\n");
        } else {
            printf("[ls] 1 PSRAM 1KB: 坏 @%d (期望 0x%02X 实际 0x%02X)\n",
                   bad, (uint8_t)(bad * 31 + 7), ps[bad]);
        }
    }

    /* 2) 内部 RAM 对照组: 它坏了说明问题不在 PSRAM 半睡, 而是整个 RTC/DRAM 不保 */
    if (!in) {
        printf("[ls] 2 内部 RAM: 分配失败 (跳过)\n");
    } else {
        int bad = -1;
        for (int i = 0; i < IN_BYTES; i++) {
            if (in[i] != (uint8_t)(i ^ 0x5A)) { bad = i; break; }
        }
        if (bad < 0) printf("[ls] 2 内部 RAM 256B: OK\n");
        else         printf("[ls] 2 内部 RAM 256B: 坏 @%d\n", bad);
    }

    /* 3) 屏幕: 整屏重画一帧, 人眼确认有没有花屏/偏移 (GRAM 与 SPI 主机状态) */
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(NULL);
    fflush(stdout);
    printf("[ls] 3 屏幕: 已整屏重画, 看有没有花屏/偏移\n");

    /* 4) 触摸: 只有 INT 触发或按住时才读 I2C, 空闲时不碰总线 */
    int32_t tx = -1, ty = -1;
    bool pressed = touch_poll(&tx, &ty);
    printf("[ls] 4 触摸: %s (%d,%d)\n", pressed ? "按下" : "空闲", (int)tx, (int)ty);

    /* 5) SD: 走 sd_fs_lock, 避免和 sys_monitor/解码器抢 FATFS */
    struct stat st;
    int sr = -1;
    sd_fs_lock();
    if (sdmmc_disk_is_mounted()) sr = stat("/sdcard", &st);
    const bool mounted = sdmmc_disk_is_mounted();
    sd_fs_unlock();
    printf("[ls] 5 SD: mounted=%d stat=%d\n", (int)mounted, sr);

    if (ps) heap_caps_free(ps);
    if (in) heap_caps_free(in);
}

/* 息屏: 淡出 + 降刷新 + 关触摸 (BT 保持连接) */
static void screen_off(void)
{
    s_state = PWR_SCREEN_OFF;
    screen_fade(0, NULL);                    /* 背光淡出到 0 */
    set_lvgl_slow_refresh(true);             /* 刷新降频省电 */
    ui_touch_set_enabled(false);             /* LVGL 触摸开关置 off */
    ESP_LOGI(TAG, "息屏 (BT 保持连接)");
}

/* 亮屏: 恢复刷新+触摸 → 淡入 */
static void screen_on(void)
{
    s_state = PWR_ACTIVE;
    power_mgr_notify_activity();             /* 亮屏后重新计时, 避免立刻再次熄屏 */
    set_lvgl_slow_refresh(false);            /* 恢复刷新频率 */
    ui_touch_set_enabled(true);              /* 恢复触摸 */
    screen_fade(brightness_get(), NULL);     /* 淡入到保存的亮度 */
    ESP_LOGI(TAG, "亮屏");
}

/* 浅睡: 屏幕已经淡出到 0 (或本来就是息屏), 触摸/刷新已关; 这里只负责睡和醒来.
 * **不碰 GPIO22** —— 浅睡期间外设一直供电, 醒来程序从断点继续.
 * 醒来两种: 定时到点 → 转深睡; 按键 → 淡入亮屏回原界面. */
static void enter_light_sleep(lv_anim_t *a)
{
    (void)a;

    s_state = PWR_LIGHT_SLEEP;
    set_lvgl_slow_refresh(true);          /* 与息屏一致, 醒来由 screen_on 恢复 */
    ui_touch_set_enabled(false);

    if (s_light_sleep_min <= 0) {         /* 设置成 0 了却还进来: 直接深睡兜底 */
        s_state = PWR_FAKE_OFF;
        enter_deep_sleep_now();
    }

    ESP_LOGI(TAG, "浅睡 %d 分钟 (按键唤醒 / 到点深睡)", s_light_sleep_min);

    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_sleep_enable_ext0_wakeup(PIN_PWR_KEY, 1);            /* 电源键高电平 */
    esp_sleep_enable_timer_wakeup((uint64_t)s_light_sleep_min * 60ULL * 1000000ULL);

    uart_flush_before_sleep();          /* 这一行必须在 esp_light_sleep_start() 之前 */

    esp_err_t err = esp_light_sleep_start();

    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);   /* 别把 5 分钟 timer 带到深睡 */

    /* 醒来时按键多半还按着: 按"已按住"记账, 否则 10ms 轮询会把这一下当短按,
     * 亮屏后立刻又睡回去 */
    key_gpio_restore();
    s_key_prev       = (gpio_get_level(PIN_PWR_KEY) == 1);
    s_key_pending    = false;
    s_key_long_fired = true;

    if (cause == ESP_SLEEP_WAKEUP_TIMER) {   /* 5 分钟到点: 走原来的关机路径 */
        ESP_LOGI(TAG, "浅睡到点, 转深睡");
        s_state = PWR_FAKE_OFF;
        enter_deep_sleep_now();              /* 不返回 */
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "浅睡异常 (%s), 直接亮屏", esp_err_to_name(err));
    }

    /* 按键唤醒: 浅睡这段不算进自动休眠的剩余量, 然后淡入亮屏 */
    autosleep_on_wake();
    screen_on();                             /* notify_activity + 恢复刷新/触摸 + 淡入 */
}

/* 屏幕已黑 (淡出完成, 或本来就息屏) 之后的统一分派: 0 分钟直接深睡, >0 先浅睡.
 * 三条入口都收在这里, 免得"0 就直睡"这个判断散落好几份. */
static void sleep_entry(void)
{
    if (s_light_sleep_min > 0) {
        enter_light_sleep(NULL);
    } else {
        s_state = PWR_FAKE_OFF;
        enter_deep_sleep(NULL);
    }
}

/* 短按/无操作路径的淡出完成回调 (淡出用的是正常背光渐暗, 不是硬切) */
static void sleep_fade_done(lv_anim_t *a)
{
    (void)a;
    sleep_entry();
}

/* 执行与之前相同的短按动作 (息屏/亮屏/浅睡) */
static void power_short_press_action(void)
{
    if (s_sleep_inhibit || s_ui_inhibit) {   /* OTA 校验窗口 / WiFi 操作中: 按键直接吞掉 */
        ESP_LOGW(TAG, "校验或 WiFi 操作中, 忽略睡眠键");
        return;
    }
    switch (s_state) {
    case PWR_ACTIVE:
        if (bt_a2dp_get_state() != BT_STATE_DISCONNECTED) {
            /* 已连接/连接中 → 手动息屏 (音频继续) */
            screen_off();
        } else {
            /* 蓝牙未连接 → 淡出后按设置浅睡 N 分钟 (0 = 直接深睡), 观感与息屏一致 */
            const bool light = (s_light_sleep_min > 0);
            s_state = light ? PWR_LIGHT_SLEEP : PWR_FAKE_OFF;
            ESP_LOGI(TAG, "BT 未连接, 淡出后%s", light ? "浅睡" : "深睡");
            screen_fade(0, sleep_fade_done);
        }
        break;
    case PWR_SCREEN_OFF:
        screen_on();   /* 息屏中按键 → 亮屏 */
        break;
    case PWR_FAKE_OFF:
        break;   /* 深睡淡出中: 忽略按键 */
    case PWR_LIGHT_SLEEP:
        break;   /* 浅睡淡出中: 忽略按键 (睡眠本身在 LVGL 任务里阻塞, 期间不会有轮询) */
    }
}

/* 自动熄屏分支时长 (分钟): 读缓存.
 * branch 取值与 settings.h 的 SETTINGS_IDLE_* 一致 (通用/小说/歌词) */
int power_mgr_idle_minutes(int branch)
{
    if (branch < 0 || branch >= SETTINGS_IDLE_NUM) branch = SETTINGS_IDLE_COMMON;
    return s_idle_min[branch];
}

/* 改某个分支的时长: 先改缓存 (立刻生效), 再落 NVS */
void power_mgr_idle_set(int branch, int minutes)
{
    if (branch < 0 || branch >= SETTINGS_IDLE_NUM) return;
    if (minutes < IDLE_MIN_MIN) minutes = IDLE_MIN_MIN;
    if (minutes > IDLE_MIN_MAX) minutes = IDLE_MIN_MAX;

    s_idle_min[branch] = minutes;
    settings_idle_minutes_save(branch, minutes);
    ESP_LOGI(TAG, "自动熄屏分支 %d → %d 分钟", branch, minutes);
}

/* 浅睡时长 (分钟, 0 = 不浅睡直接深睡): 读缓存 */
int power_mgr_light_sleep_minutes(void)
{
    return s_light_sleep_min;
}

/* 改浅睡时长: 先改缓存 (下一次进睡就用新值), 再落 NVS */
void power_mgr_light_sleep_set_minutes(int minutes)
{
    if (minutes < LIGHT_SLEEP_MIN_MIN) minutes = LIGHT_SLEEP_MIN_MIN;
    if (minutes > LIGHT_SLEEP_MIN_MAX) minutes = LIGHT_SLEEP_MIN_MAX;

    s_light_sleep_min = minutes;
    settings_light_sleep_minutes_save(minutes);
    ESP_LOGI(TAG, "浅睡时长 → %d 分钟%s", minutes, minutes ? "" : " (直接深睡)");
}

/* 定时休眠到点 (autosleep 调用): 走与"蓝牙未连接时短按电源键"同一条淡出→深睡路径,
 * 但**不看蓝牙状态** —— 用户要的是到点就关机 (GPIO22 断电, 音乐结束), 不是只熄屏.
 * 抑制标志与短按同规矩: 校验窗口/WiFi 流程期间不睡, 免得把升级打断. */
void power_mgr_sleep_now(void)
{
    if (s_sleep_inhibit || s_ui_inhibit) {
        ESP_LOGW(TAG, "抑制中 (OTA/WiFi), 忽略定时休眠");
        return;
    }
    if (s_mode_switching || s_state == PWR_FAKE_OFF || s_state == PWR_LIGHT_SLEEP) return;

    s_state = PWR_FAKE_OFF;
    ESP_LOGI(TAG, "定时休眠到点, 淡出后深睡");
    screen_fade(0, enter_deep_sleep);
}

/* 无操作熄屏轮询 (1s): 亮屏且超过时限时, 执行与睡眠键短按相同的动作.
 * 菜单/设置 30 秒, 音乐 2 分钟, 歌词与小说 5 分钟; 息屏、深睡淡出、
 * 模式切换过渡中均不处理. */
static void idle_poll_cb(lv_timer_t *timer)
{
    (void)timer;

    /* Phase 0 诊断: console 的 lsp <秒> 请求在这里执行 (必须同 LVGL 任务) */
    int test_sec = s_ls_test_sec;
    if (test_sec > 0) {
        s_ls_test_sec = 0;
        light_sleep_test_run(test_sec);
        return;
    }

    if (s_mode_switching) return;
    if (s_sleep_inhibit || s_ui_inhibit) return;   /* OTA 校验 / WiFi 操作中: 不熄屏 */
    if (s_state != PWR_ACTIVE) return;

    int64_t timeout_us;
    {
        /* 只列举两条特殊分支, 其余一律走通用 —— 以后加新模式不用再动这里 */
        int br;
        if (app_mode_is_novel()) br = SETTINGS_IDLE_NOVEL;
        else if (s_lyrics_active) br = SETTINGS_IDLE_LYRICS;
        else                      br = SETTINGS_IDLE_COMMON;

        timeout_us = (int64_t)s_idle_min[br] * 60LL * 1000 * 1000;
    }
    int64_t now = esp_timer_get_time();
    if (now - s_last_activity_us < timeout_us) return;

    s_last_activity_us = now;   /* 先续期, 动作/淡出期间不重复触发 */
    ESP_LOGI(TAG, "无操作 %d 分钟, 自动执行睡眠键短按动作",
             (int)(timeout_us / 60000000LL));
    power_short_press_action();
}

/* 按键电平上报 (复用 vol_key 10ms 定时器).
 * 上升沿只记时; 按住 >=800ms 在亮屏态进入主菜单 (已在菜单则无反应);
 * 松手未触发长按则执行短按动作. */
void power_mgr_poll_key(bool level)
{
    if (s_mode_switching) return;   /* 切换过渡中: 忽略 */
    if (s_sleep_inhibit || s_ui_inhibit) {   /* 校验/WiFi 操作中: 吞掉按键, 但仍跟踪电平避免解锁后误判边沿 */
        s_key_prev = level;
        return;
    }

    int64_t now = esp_timer_get_time();
    bool rising  = level && !s_key_prev;
    bool falling = !level && s_key_prev;
    s_key_prev = level;

    if (rising) {
        power_mgr_notify_activity();   /* 电源键按下也算一次操作 */
        s_key_press_us   = now;
        s_key_long_fired = false;
        s_key_pending    = true;
        return;
    }

    /* 按住判定长按: 仅亮屏态且尚未处理 */
    if (level && s_key_pending && !s_key_long_fired && s_state == PWR_ACTIVE) {
        if (now - s_key_press_us >= PWR_KEY_LONG_US) {
            s_key_long_fired = true;
            s_key_pending    = false;
            ESP_LOGI(TAG, "长按睡眠键: 进入主菜单");
            app_mode_request(APP_MODE_MENU);   /* 已在菜单里则被拦掉, 无反应 */
        }
        return;
    }

    if (falling && s_key_pending) {
        s_key_pending = false;
        if (s_key_long_fired) return;   /* 长按已处理, 松手不再触发短按 */

        /* 冷却窗口: 两次沿间隔 <200ms 视为抖动, 丢弃 */
        if (s_key_last_us != 0 && (now - s_key_last_us) < PWR_KEY_COOLDOWN_MS * 1000LL) {
            return;
        }
        s_key_last_us = now;

        /* 设置页里短按 = 返回上一级 (列表那一级 = 退出设置回菜单).
         * 注意这里拦的是"物理按键"这一条; 无操作熄屏仍然走 power_short_press_action,
         * 否则在设置页里闲置 30 秒会莫名其妙地"返回". */
        if (app_mode_is_settings()) {
            ui_settings_back();
            return;
        }
        power_short_press_action();
    }
}

static void mode_switch_finish(void);

/* 把整屏刷成纯黑 (背光此时是 0, 看不见; 但能保证之后无论哪一帧漏画,
 * 底下的 GRAM 都是黑的, 绝不会露出上一屏的白底) */
static void panel_fill_black(void)
{
    enum { ROWS = 16, BYTES = 172 * ROWS * 2 };
    uint8_t *buf = heap_caps_malloc(BYTES, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!buf) {
        ESP_LOGW(TAG, "黑场刷黑: 缓冲分配失败 (跳过)");
        return;
    }
    memset(buf, 0, BYTES);
    for (int y = 0; y < TFT_VER_RES; y += ROWS) {
        int rows = (y + ROWS > TFT_VER_RES) ? (TFT_VER_RES - y) : ROWS;
        lcd_draw_bitmap_sync(0, y, TFT_HOR_RES, y + rows, buf);
    }
    heap_caps_free(buf);
}

/* 淡出完成 (背光已到 0, 屏幕全黑): 清理旧界面 + 建新界面, 再淡入.
 * 三个动作(刷黑/清理/建界面)全都发生在全黑期间 —— 背光在这里再显式写一次 0,
 * 不依赖上一步"应该已经到 0"; 面板也显式刷黑一遍, 免得旧页的像素留在 GRAM 里. */
static void mode_switch_dark_done(void)
{
    if (s_cur_bri != 0) {
        s_cur_bri = 0;
        lcd_set_brightness(0);
    }
    ESP_LOGI(TAG, "黑场: 开始刷黑/清理/建界面");

    panel_fill_black();
    if (s_switch_work) s_switch_work();

    /* 全黑下把新界面同步推入帧缓冲, 否则亮起来的头一帧还是旧画面.
     * (band 还在 SPI 上飞也无所谓, 这时候背光还是 0)
     * 下面这行日志用来确认"待刷区域"确实覆盖整屏: 如果 inv_p=0 或面积不对,
     * 就会有一块旧页(比如设置页的白底)留在 GRAM 里, 淡入时闪一下。 */
    lv_disp_t *d = lv_disp_get_default();
    if (d) {
        ESP_LOGI(TAG, "黑场: 待刷区域 inv_p=%d 首块 (%d,%d)-(%d,%d)",
                 d->inv_p, d->inv_areas[0].x1, d->inv_areas[0].y1,
                 d->inv_areas[0].x2, d->inv_areas[0].y2);
    }
    lv_refr_now(NULL);

    ESP_LOGI(TAG, "黑场: 新界面已就绪, 开始淡入");
    bri_fade_start(brightness_get(), MODE_FADE_IN_MS, mode_switch_finish);
}

/* 过渡结束: 解除切换屏蔽, 恢复触摸 (光圈路径和背光路径共用) */
static void mode_switch_finish(void)
{
    s_mode_switching = false;
    ui_touch_set_enabled(true);
    power_mgr_notify_activity();   /* 切换完成重新计时 */
}

/* 模式切换过渡入口.
 *
 * backlight_fade=true: 背光淡出到 0 → 清理/建页 → 淡回当前亮度. 过渡期间屏幕上
 *   没有一个像素在变, 所以分带写入的接缝根本无从出现 —— 这是"彻底没有撕裂"的
 *   唯一办法. 设置页整屏大白底, 黑白交界最长, 连纯透明度淡化都能看见一根灰线在
 *   Y 上乱跳(相邻两条带差一档亮度), 所以它走这条.
 * backlight_fade=false: 光圈缩放合成动画 (菜单/音乐/小说); 它们的底色本来就黑,
 *   接缝看不出来, 值得保留光圈的观感. 资源不足时同样降级到背光淡化. */
void power_mgr_mode_transition(void (*work)(void), bool backlight_fade)
{
    s_mode_switching = true;
    s_switch_work    = work;
    ui_touch_set_enabled(false);   /* 过渡期间关触摸, 避免误触 */

    if (!backlight_fade && mode_anim_run(work, mode_switch_finish)) {
        return;   /* 已由动画接管, 结束后回调 mode_switch_finish */
    }

    /* 背光淡出 → 黑场里切换 → 淡入 */
    bri_fade_start(0, MODE_FADE_OUT_MS, mode_switch_dark_done);
}

/* 息屏期 BT 状态轮询 (1s): 从已连/连接中变为未连接 → 浅睡 (到点转深睡) */
static void bt_poll_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_mode_switching) return;            /* 模式切换过渡中不处理 */
    if (s_state != PWR_SCREEN_OFF) return;   /* 非息屏态不处理 */
    if (bt_a2dp_get_state() == BT_STATE_DISCONNECTED) {
        ESP_LOGI(TAG, "息屏期间蓝牙断开, 进入%s",
                 s_light_sleep_min > 0 ? "浅睡" : "深睡");
        sleep_entry();                       /* 屏幕已黑, 不用再淡出 */
    }
}

/* 早启 (app_main 第一行, LCD 前半段之前): 释放深睡保持 + GPIO22 拉低开启外设供电.
 * 冷启动时 bootloader 已拉低 IO22, 此处为深睡唤醒兜底 (唤醒后 hold 仍保持高电平). */
void power_mgr_early_init(void)
{
    gpio_deep_sleep_hold_dis();          /* 关闭全局深睡引脚保持, 允许重新配置 */
    gpio_hold_dis(PIN_PWR_CUT);          /* 释放 GPIO22 锁定 */
    gpio_config_t cut_cfg = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << PIN_PWR_CUT,
    };
    gpio_config(&cut_cfg);
    gpio_set_level(PIN_PWR_CUT, 0);      /* 低 = 外设供电开启 */
}

/* 电源管理初始化 (在 LVGL 任务中调用):
 * 配置按键输入 + 开机背光渐入 + 启动 BT 状态轮询 */
void power_mgr_init(void)
{
    /* GPIO37 按键输入 (外部已有 10k 下拉, ESP32 输入脚无内部上下拉) */
    gpio_config_t key_cfg = {
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = 1ULL << PIN_PWR_KEY,
    };
    gpio_config(&key_cfg);

    /* 初始电平作为上次值: 开机时按钮若仍按住, 需等一次释放后再按才触发 */
    s_key_prev = (gpio_get_level(PIN_PWR_KEY) == 1);
    s_key_last_us = 0;
    s_last_activity_us = esp_timer_get_time();   /* 开机即开始无操作计时 */

    /* 开机背光渐入: 从 0 渐升到保存亮度 (复用 screen_fade) */
    s_cur_bri = 0;
    lcd_set_brightness(0);
    screen_fade(brightness_get(), NULL);

    /* 自动熄屏的三个时长: 开机从 NVS 灌进缓存, 1 秒轮询里不再碰 flash */
    for (int i = 0; i < SETTINGS_IDLE_NUM; i++) {
        s_idle_min[i] = settings_idle_minutes_load(i);
    }
    s_light_sleep_min = settings_light_sleep_minutes_load();   /* 浅睡时长同理由缓存提供 */

    /* 息屏期 BT 状态轮询 + 无操作熄屏轮询: 均 1 秒一次 */
    lv_timer_create(bt_poll_cb, 1000, NULL);
    lv_timer_create(idle_poll_cb, PWR_IDLE_POLL_MS, NULL);
}

/* ────────────────────────── 电池电压 ADC (开机检测) ────────────────────────── */

/* 初始化 ADC 单元/通道/校准 (幂等: 已初始化则跳过).
 * 由 power 独占, sys_monitor 周期采样通过 power_mgr_vbat_read_once 复用.
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
 * 供开机检测 (取平均) 及 sys_monitor 周期采样共用.
 * 返回电压(V); 返回负值表示 ADC 不可用/读取失败 (调用方不得据此判定低压) */
float power_mgr_vbat_read_once(void)
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

/* 开机低电量检测 (app_main 在 lcd_init_early 之后调用):
 * 初始化 ADC → 3 次采样平均 → 写入全局 g_vbat.
 * 平均电压 ≥ 阈值: 直接返回, 走正常启动 (lcd_init_finish 由 LVGL 任务完成);
 * 平均电压 < 阈值: 低电量分支, 完成 LCD 初始化后整屏写低电图标,
 * 亮度拉高到 128, 停留 2 秒, 拉低背光, 最后进入深度睡眠 (不返回). */
void power_mgr_boot_battery_check(void)
{
    if (!vbat_adc_init()) {
        ESP_LOGW(TAG, "电池 ADC 不可用, 跳过开机低压检测 (按正常启动)");
        return;   /* 不得据 0V 判定低电, 否则会误深睡 */
    }

    float sum = 0.0f;
    int   valid = 0;
    for (int i = 0; i < VBAT_BOOT_SAMPLES; i++) {
        float v = power_mgr_vbat_read_once();
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
    enter_deep_sleep_now();             /* 进入深度睡眠 (不返回) */
}

/* 紧急关机 (运行中电压过低): 无动画直接深睡.
 * 拉低背光 → 切外设供电 → 设置唤醒 → 深睡 (不返回).
 * 调用场景: sys_monitor 周期采样发现 g_vbat < 临界值 */
void power_mgr_critical_shutdown(void)
{
    ESP_LOGW(TAG, "电压过低, 紧急关机进入深度睡眠");
    lcd_set_brightness(0);              /* 直接拉低背光 (无淡出) */
    vTaskDelay(pdMS_TO_TICKS(20));      /* 留出背光熄灭时间 */
    enter_deep_sleep_now();             /* 切外设供电 + 唤醒 + 深睡 (不返回) */
}
