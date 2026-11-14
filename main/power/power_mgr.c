/* 电源管理: 电源键 (GPIO37) + 三级睡眠梯 + 无操作自动熄屏.
 *
 * 睡眠不是三个并列的分支, 而是同一件事的三个档位 —— 只能顺着往下走:
 *
 *   亮屏  PWR_ON
 *     │ 一级 pwr_off()    背光淡出到 0, 触摸关, LVGL 刷新降到 100s
 *     ▼
 *   灭屏  PWR_OFF          蓝牙/音频照常, 按键立刻淡入回来 (不算睡)
 *     │ 二级 pwr_down()
 *     ▼
 *   浅睡                  CPU/AHB/APB 停钟, 外设一直供电; 按键唤醒后从断点继续
 *     │ 定时到点 / 未启用浅睡
 *     ▼
 *   深睡                  GPIO22 断电, ext0 唤醒, 不返回
 *
 * 三条规矩:
 *   1) 事件只从四个源来 (按键/无操作/蓝牙断开/自动休眠), 每个源只做"判断条件 +
 *      报事件", 自己绝不挑动作 —— 所以"要不要浅睡""要不要看蓝牙"只有决策表里一份.
 *   2) 状态只由一级/二级/三级的执行体写, 事件源一律只读.
 *   3) 淡入淡出只有 power_fade 一份实现; 这里只决定"淡到哪儿、淡完干什么".
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_heap_caps.h"
#include "esp_rom_uart.h"
#include "lvgl.h"
#include "drv_display.h"
#include "settings.h"
#include "bt_a2dp.h"
#include "autosleep.h"
#include "ui_core.h"
#include "mode_anim.h"
#include "app_mode.h"
#include "ui_player.h"
#include "ui_settings.h"
#include "power_fade.h"
#include "power_mgr.h"

static const char *TAG = "POWER";

/* GPIO37: 息屏/唤醒按键 (外部 10k 下拉, 仅输入, 只测上升沿) */
#define PIN_PWR_KEY      GPIO_NUM_37
/* GPIO22: 外设供电控制 (高=切断外设供电), 深睡时保持 */
#define PIN_PWR_CUT      GPIO_NUM_22

/* 息屏时 LVGL 刷新周期: 100 秒一次 (省电, 屏幕已黑无需频繁刷新) */
#define LVGL_SCREEN_OFF_REFRESH_MS  100000

/* 设置页进出: 背光淡出/淡入的时长. 和光圈动画的 150/180ms 对齐,
 * 否则按亮度差映射会拉到 400ms 一程, 满亮度下整个切换 0.8s 太长 */
#define MODE_FADE_OUT_MS      150
#define MODE_FADE_IN_MS       180

/* 按键冷却: 吞机械抖动产生的二次上升沿 */
#define PWR_KEY_COOLDOWN_MS  200
/* 长按判定: 按住超过此值进入主菜单 */
#define PWR_KEY_LONG_US      (800 * 1000)

/* 无操作熄屏轮询周期 */
#define PWR_WATCH_MS         1000

/* 自动熄屏: "通用 + 特殊"两层次, 单位分钟 (1~10, 设置页可改).
 *   通用 = 菜单 / 设置 / 音乐封面, 以及以后新加的任何模式;
 *   特殊 = 小说、音乐歌词页 (各自覆盖通用).
 * 这里缓存一份 (1 秒轮询每次都读, 不能读 NVS), 开机由 power_mgr_init 灌入. */
static int s_idle_min[SETTINGS_IDLE_NUM] = { 2, 5, 5 };   /* 与 settings.c 的默认值一致 */

/* 浅睡时长 (分钟, 0 = 不浅睡直接深睡): 同上, 缓存在这里, 睡眠路径上不读 flash */
static int s_light_sleep_min = LIGHT_SLEEP_MIN_DEFAULT;

/* 电源状态 (只有三个; 淡出/浅睡都不是独立状态, 都归在"下电中") */
typedef enum {
    PWR_ON,    /* 亮屏 */
    PWR_OFF,   /* 灭屏但活着: 背光 0、触摸关、刷新降频; 蓝牙/音频照常 */
    PWR_DOWN,  /* 下电中: 淡出 → 浅睡 → 深睡; 出口只有"醒"这一条 */
} pwr_state_t;

static pwr_state_t s_state = PWR_ON;         /* 当前电源状态 */
static bool        s_down_light = false;     /* 本次下电允许走浅睡这一级 (自动休眠不允许) */

static bool        s_key_prev = false;       /* 上次按键电平 (用于检测沿) */
static int64_t     s_key_last_us = 0;        /* 上次按键沿的时刻 (us), 用于冷却 */
static int64_t     s_key_press_us = 0;       /* 本次按下的时刻 (us), 长按判定用 */
static bool        s_key_pending = false;    /* 已按下且尚未判定 (短按待松手触发) */
static bool        s_key_long_fired = false; /* 本次长按已处理 */
static int64_t     s_last_activity_us = 0;   /* 最近一次用户操作时刻 (us), 无操作熄屏用 */
static bool        s_lyrics_active = false;  /* 音乐模式下当前是否显示歌词页 (走"歌词"分支) */

/* 睡眠抑制: OTA 新固件校验窗口期间置位.
 * 窗口内电源键短按/长按、所有自动睡眠全部被拦下 —— 用户只会看到遮罩上的
 * "请勿断电", 按什么都没反应, 避免误触导致新固件来不及打标就被回退. */
static volatile bool s_sleep_inhibit = false;

/* 界面抑制: 设置页的 WiFi 流程在扫描/连接/释放期间置位.
 * 和 s_sleep_inhibit 分开存、互不覆盖 (OTA 的校验窗口可能同时也在用),
 * 两者任一为真 → 电源键短按/长按与所有自动睡眠全部失效. */
static volatile bool s_ui_inhibit = false;

/* 模式切换状态 */
static bool  s_mode_switching = false;     /* 切换过渡中: 屏蔽按键/睡眠事件 */
static void (*s_switch_work)(void) = NULL; /* 切换时黑屏窗口内执行的工作 */

/* ── 梯子的四个执行体 (定义见下面"三级睡眠梯", 决策表要用, 先声明) ── */
static void pwr_off(void (*after)(void));
static void pwr_wake(void);
static void pwr_down(bool allow_light);
static void pwr_down_after_dark(void);
static bool pwr_light_sleep(int minutes);

/* ═══════════════════════════ 1. 前提动作 (梯子用的工具) ═══════════════════════════ */

/* LVGL 刷新频率: slow=true 降为 100s 一次, false 恢复默认 20ms */
static void set_lvgl_slow_refresh(bool slow)
{
    lv_timer_t *refr = _lv_disp_get_refr_timer(lv_disp_get_default());
    if (!refr) return;
    if (slow) {
        lv_timer_set_period(refr, LVGL_SCREEN_OFF_REFRESH_MS);   /* 息屏: 刷新周期拉长 */
    } else {
        lv_timer_set_period(refr, LV_DISP_DEF_REFR_PERIOD);      /* 亮屏: 恢复默认周期 */
        lv_timer_resume(refr);                                   /* 确保定时器处于运行态 */
    }
}

/* 屏幕已经黑掉了: 没人再看, 省电 (刷新降频 + 关触摸). 幂等, 灭屏和浅睡都调它 */
static void pwr_ui_dim(void)
{
    set_lvgl_slow_refresh(true);
    ui_touch_set_enabled(false);
}

/* 屏幕要亮起来: 恢复刷新 + 触摸. 幂等 */
static void pwr_ui_live(void)
{
    set_lvgl_slow_refresh(false);
    ui_touch_set_enabled(true);
}

/* ext0 唤醒会把 GPIO37 切成 RTC IO, 浅睡醒来后必须改回普通数字输入 ——
 * 否则 gpio_get_level 读到的一直是 0, 电源键从此"按不动"
 * (深睡走重启, GPIO 会重新初始化, 不会碰到这个问题). */
static void key_gpio_restore(void)
{
    gpio_config_t key_cfg = {
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = 1ULL << PIN_PWR_KEY,
    };
    gpio_config(&key_cfg);
}

/* 睡眠醒来后的按键记账: 醒来时按键多半还按着, 按"已按住 + 长按已处理"记账,
 * 否则 10ms 轮询会把这一下判成短按/长按, 亮屏后立刻又睡回去或跳进菜单 */
static void sleep_key_after_wake(void)
{
    key_gpio_restore();
    s_key_prev       = (gpio_get_level(PIN_PWR_KEY) == 1);
    s_key_pending    = false;
    s_key_long_fired = true;
}

/* 睡前把串口吐干净: 一进浅睡 UART 时钟就停了, 还在驱动/FIFO 里没发出去的字节会被
 * 截断 —— 日志里只剩半句"浅睡 5 分钟"就是这个原因. 必须在 esp_light_sleep_start()
 * 之前调用 */
static void uart_flush_before_sleep(void)
{
    fflush(stdout);
    fsync(STDOUT_FILENO);                    /* stdio/VFS 那一层也刷掉 */
    esp_rom_output_tx_wait_idle(0);          /* UART0: 等到 TX FIFO 真的空 */
}

/* 配置唤醒源: 先清掉所有 (浅睡留下的 timer 不清, 深睡 5 分钟后会自己醒来),
 * 再挂电源键高电平唤醒, timer_sec>0 时再加一个定时唤醒.
 * 浅睡和深睡共用 —— 深睡传 0 */
static void sleep_wake_arm(int timer_sec)
{
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_sleep_enable_ext0_wakeup(PIN_PWR_KEY, 1);
    if (timer_sec > 0) {
        esp_sleep_enable_timer_wakeup((uint64_t)timer_sec * 1000000ULL);
    }
}

/* ═══════════════════════ 2. 事件 → 决策 → 执行 (唯一策略表) ═══════════════════════ */

/* 决策表能输出的动作. 每个动作对应梯子上的一条固定路线 */
typedef enum {
    PWR_DO_NONE,    /* 什么都不做 (抑制中 / 已在下电 / 不该睡) */
    PWR_DO_ON,      /* 亮屏 */
    PWR_DO_OFF,     /* 一级: 只灭屏 (蓝牙音频继续) */
    PWR_DO_SLEEP,   /* 一级 → [二级 浅睡] → 三级: 淡出后按设置浅睡, 到点深睡 */
    PWR_DO_SHUT,    /* 一级 → 三级: 淡出后直接深睡 (自动休眠: 到点就关机) */
} pwr_do_t;

static const char *const EV_NAME[] = { "KEY", "IDLE", "BT_LOST", "TIMER" };  /* 顺序同 pwr_ev_t */
static const char *const DO_NAME[] = { "NONE", "ON", "OFF", "SLEEP", "SHUT" };

static const char *pwr_state_name(void)
{
    switch (s_state) {
    case PWR_ON:  return "亮屏";
    case PWR_OFF: return "灭屏";
    default:      return "下电中";
    }
}

/* 唯一的策略: (事件, 状态, 蓝牙, 设置) → 动作.
 * 三行闸门之后, 每种事件该干什么一眼看完; 以后要改行为只改这一张表 */
static pwr_do_t pwr_decide(pwr_ev_t ev)
{
    if (power_mgr_sleep_inhibited())     return PWR_DO_NONE;   /* OTA 校验 / WiFi 流程 */
    if (s_mode_switching)                return PWR_DO_NONE;   /* 模式切换过渡中 */
    if (s_state == PWR_DOWN)             return PWR_DO_NONE;   /* 已经在下电, 别插队 */

    switch (ev) {
    case PWR_EV_KEY:
        if (s_state == PWR_OFF) return PWR_DO_ON;              /* 灭屏 → 亮屏 */
        if (bt_a2dp_get_state() != BT_STATE_DISCONNECTED) {
            return PWR_DO_OFF;                                 /* 有声: 只熄屏, 音频继续 */
        }
        return PWR_DO_SLEEP;                                   /* 无声: 整条梯放下去 */
    case PWR_EV_IDLE:
        return pwr_decide(PWR_EV_KEY);                         /* 超时 = 替用户按一下 */
    case PWR_EV_BT_LOST:
        return (s_state == PWR_OFF) ? PWR_DO_SLEEP : PWR_DO_NONE;
    case PWR_EV_TIMER:
        return PWR_DO_SHUT;                                    /* 自动休眠: 不看蓝牙 */
    }
    return PWR_DO_NONE;
}

void power_mgr_sleep_event(pwr_ev_t ev)
{
    pwr_do_t d = pwr_decide(ev);

    ESP_LOGI(TAG, "事件 %s | 状态 %s | BT %s → %s",
             EV_NAME[ev], pwr_state_name(),
             bt_a2dp_get_state() == BT_STATE_DISCONNECTED ? "断开" : "已连",
             DO_NAME[d]);

    switch (d) {
    case PWR_DO_NONE:  break;
    case PWR_DO_ON:    pwr_wake();     break;
    case PWR_DO_OFF:   pwr_off(NULL);  break;
    case PWR_DO_SLEEP: pwr_down(true); break;
    case PWR_DO_SHUT:  pwr_down(false); break;
    }
}

/* ═════════════════════════════ 3. 三级睡眠梯 ═════════════════════════════ */

/* 一级: 灭屏. 全工程唯一的"关背光"动作 —— 息屏、浅睡前、深睡前都走它.
 * after != NULL 表示这是下电序列的第一级, 淡出完成后继续往下走 (掉到二级) */
static void pwr_off(void (*after)(void))
{
    s_state = after ? PWR_DOWN : PWR_OFF;
    pwr_ui_dim();
    ESP_LOGI(TAG, "一级 灭屏%s", after ? " (继续下电)" : "");
    pwr_fade_to(0, after);
}

/* 唯一的"亮屏"动作 */
static void pwr_wake(void)
{
    s_state = PWR_ON;
    power_mgr_notify_activity();             /* 亮屏后重新计时, 免得太快又被超时熄掉 */
    pwr_ui_live();
    pwr_fade_to(brightness_get(), NULL);     /* 淡入到保存的亮度 */
    ESP_LOGI(TAG, "亮屏");
}

/* 二级: 下电. 唯一的关机序列, 三条入口 (短按/超时/蓝牙断开) 全收在这里.
 * allow_light=false 时不走浅睡这一级 (自动休眠到点就是关机, 不该再给人 5 分钟) */
static void pwr_down(bool allow_light)
{
    if (s_state == PWR_DOWN) return;
    s_down_light = allow_light;

    if (pwr_fade_is_dark()) {                /* 已经息屏: 不重复淡出 */
        s_state = PWR_DOWN;
        pwr_ui_dim();
        pwr_down_after_dark();
        return;
    }
    pwr_off(pwr_down_after_dark);            /* 淡出完成后进 pwr_down_after_dark */
}

/* 屏幕已黑之后的分派: 浅睡 N 分钟 (0 = 不浅睡) → 深睡 */
static void pwr_down_after_dark(void)
{
    const int min = power_mgr_light_sleep_minutes();

    if (!s_down_light || min <= 0) {
        power_mgr_shutdown_now();            /* 三级: 不返回 */
    }

    if (pwr_light_sleep(min)) {              /* 二级: 被按键叫醒了 */
        pwr_wake();
        return;
    }
    ESP_LOGI(TAG, "浅睡到点, 转深睡");
    power_mgr_shutdown_now();                /* 三级: 不返回 */
}

/* 二级: 浅睡. 前提 = 屏幕已黑、触摸已关、外设仍供电; 醒来程序从断点继续.
 * 返回 true = 被按键叫醒 (调用方负责亮屏), false = 定时到点 (调用方去深睡) */
static bool pwr_light_sleep(int minutes)
{
    ESP_LOGI(TAG, "二级 浅睡 %d 分钟 (按键唤醒 / 到点深睡)", minutes);

    sleep_wake_arm(minutes * 60);
    uart_flush_before_sleep();               /* 必须在 esp_light_sleep_start() 之前 */
    esp_err_t err = esp_light_sleep_start();

    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);   /* 别把 timer 带到深睡 */
    sleep_key_after_wake();
    autosleep_on_wake();                     /* 浅睡这段不算进自动休眠的剩余量 */

    if (err != ESP_OK) {                     /* 睡失败了: 先亮屏, 别把人关在黑屏里 */
        ESP_LOGW(TAG, "浅睡异常 (%s)", esp_err_to_name(err));
        return true;
    }
    if (cause == ESP_SLEEP_WAKEUP_TIMER) return false;

    ESP_LOGI(TAG, "按键唤醒");
    return true;
}

/* 三级: 深睡. GPIO22 拉高+保持 (切断外设供电), 只剩 GPIO37 高电平唤醒 (不返回) */
void power_mgr_shutdown_now(void)
{
    s_state = PWR_DOWN;
    ESP_LOGI(TAG, "三级 深睡 (GPIO22 高电平保持, GPIO37 高电平唤醒)");

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

    sleep_wake_arm(0);                     /* 先清所有唤醒源, 再只留 GPIO37 */
    esp_deep_sleep_start();                /* 进入深睡 (此处不返回) */
}

/* ═════════════════════════════ 4. 事件源 (只报事件) ═════════════════════════════ */

/* 按键电平上报 (复用 vol_key 10ms 定时器).
 * 上升沿只记时; 按住 >=800ms 在亮屏态进入主菜单 (已在菜单则无反应);
 * 松手未触发长按则报一个睡眠事件. */
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
    if (level && s_key_pending && !s_key_long_fired && s_state == PWR_ON) {
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

        /* 设置页里短按 = 返回上一级 (列表那一级 = 退出设置回菜单). 这是按键语义,
         * 不是睡眠事件 —— 注意只拦物理按键这一条, 无操作超时照样走决策表,
         * 否则在设置页里闲置几分钟会莫名其妙地"返回". */
        if (app_mode_is_settings()) {
            ui_settings_back();
            return;
        }
        power_mgr_sleep_event(PWR_EV_KEY);
    }
}

/* 当前该用哪个分支的超时 (通用/小说/歌词): 只列两条特殊分支, 其余一律通用 ——
 * 以后加新模式不用再动这里 */
static int pwr_idle_branch(void)
{
    if (app_mode_is_novel()) return SETTINGS_IDLE_NOVEL;
    if (s_lyrics_active)     return SETTINGS_IDLE_LYRICS;
    return SETTINGS_IDLE_COMMON;
}

/* 1 秒看门: 灭屏期蓝牙断开 → 下电; 亮屏无操作超时 → 替用户按一下.
 * 抑制/过渡期间直接不看不报 (免得每秒刷一行"→ NONE") */
static void pwr_watch_cb(lv_timer_t *timer)
{
    (void)timer;

    if (s_mode_switching)                return;
    if (power_mgr_sleep_inhibited())     return;

    if (s_state == PWR_OFF && bt_a2dp_get_state() == BT_STATE_DISCONNECTED) {
        power_mgr_sleep_event(PWR_EV_BT_LOST);
        return;
    }

    if (s_state != PWR_ON) return;

    const int64_t timeout_us = (int64_t)s_idle_min[pwr_idle_branch()] * 60LL * 1000000LL;
    const int64_t now        = esp_timer_get_time();
    if (now - s_last_activity_us < timeout_us) return;

    s_last_activity_us = now;   /* 先续期: 淡出期间不重复触发 */
    ESP_LOGI(TAG, "无操作 %d 分钟", (int)(timeout_us / 60000000LL));
    power_mgr_sleep_event(PWR_EV_IDLE);
}

/* ═════════════════════════ 5. 设置 / 抑制 / 状态接口 ═════════════════════════ */

/* 记录一次用户操作: 触摸层每次读到触点、按键按下/步进时调用.
 * 所有调用方都在 LVGL 任务上下文, 与 idle 定时器同任务, 无需加锁. */
void power_mgr_notify_activity(void)
{
    s_last_activity_us = esp_timer_get_time();
}

/* 睡眠抑制开关 (OTA 校验窗口用): inhibit=true 时所有睡眠入口全部失效.
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

/* 界面抑制开关 (设置页 WiFi 流程用): 只影响"按键 + 自动睡眠",
 * 与 OTA 的 sleep_inhibit 各存一个标志, 谁也不会把对方清掉. */
void power_mgr_set_ui_inhibit(bool inhibit)
{
    if (s_ui_inhibit == inhibit) return;
    s_ui_inhibit = inhibit;
    ESP_LOGI(TAG, "界面抑制(按键/熄屏) %s", inhibit ? "开启" : "解除");
    if (!inhibit) power_mgr_notify_activity();   /* 解除时重新计时, 避免立刻熄屏 */
}

/* 歌词页显示状态: 只决定无操作超时用哪个分支的时长.
 * 由 UI 在进入/退出歌词页及销毁音乐组时调用, 与 idle 定时器同任务. */
void power_mgr_set_lyrics_active(bool active)
{
    if (s_lyrics_active == active) return;
    s_lyrics_active = active;
    power_mgr_notify_activity();   /* 模式变化后重新计时 */
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

/* ═══════════════════════════ 6. 模式切换的黑场过渡 ═══════════════════════════ */

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
    if (!pwr_fade_is_dark()) {
        pwr_fade_set(0);
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
    pwr_fade_to_ms(brightness_get(), MODE_FADE_IN_MS, mode_switch_finish);
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
    pwr_fade_to_ms(0, MODE_FADE_OUT_MS, mode_switch_dark_done);
}

/* ═══════════════════════════════ 7. 初始化 ═══════════════════════════════ */

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
 * 配置按键输入 + 开机背光渐入 + 灌入设置缓存 + 启动 1 秒看门定时器 */
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

    /* 开机背光渐入: 从 0 渐升到保存亮度 */
    pwr_fade_set(0);
    lcd_set_brightness(0);
    pwr_fade_to(brightness_get(), NULL);

    /* 自动熄屏的三个时长 + 浅睡时长: 开机从 NVS 灌进缓存, 轮询里不再碰 flash */
    for (int i = 0; i < SETTINGS_IDLE_NUM; i++) {
        s_idle_min[i] = settings_idle_minutes_load(i);
    }
    s_light_sleep_min = settings_light_sleep_minutes_load();

    lv_timer_create(pwr_watch_cb, PWR_WATCH_MS, NULL);
}
