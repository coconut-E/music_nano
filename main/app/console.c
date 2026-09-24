#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/timers.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "esp_debug_helpers.h"
#include "esp_private/freertos_debug.h"
#include "xtensa_context.h"
#include "app.h"
#include "sys_monitor.h"

#define LINE_BUF_SIZE 64   /* 串口命令行缓冲长度 */

static QueueHandle_t s_app_cmd_queue = NULL;   /* 应用命令队列 (解析出的命令投递到这里) */

/* CPU 占用统计的状态: 保存上一次定时采样的任务状态, 供 cmd_stats 计算两次采样间的增量 */
static TaskStatus_t *s_stats_prev       = NULL;   /* 上次采样的任务状态数组 */
static UBaseType_t   s_stats_prev_count = 0;      /* 上次采样到的任务个数 */
static uint32_t      s_stats_prev_total = 0;      /* 上次采样的总运行时间计数 */
static TickType_t    s_stats_prev_tick  = 0;      /* 上次采样的 tick */
static SemaphoreHandle_t s_stats_mux    = NULL;   /* 保护上面的快照数据 (互斥量, 允许锁外释放/分配) */

/* 定时器回调: 每秒采样一次全任务状态, 覆盖式保存到 s_stats_prev.
 * 分配/释放在锁外进行, 仅指针交换在锁内, 避免在临界区调用 heap */
static void stats_timer_cb(TimerHandle_t xTimer)
{
    UBaseType_t n = uxTaskGetNumberOfTasks();          /* 当前任务总数 */
    TaskStatus_t *snap = malloc(n * sizeof(TaskStatus_t));
    if (!snap) return;

    uint32_t total = 0;                                /* 输出: 采样时刻的总运行时间计数 */
    n = uxTaskGetSystemState(snap, n, &total);         /* 获取所有任务状态快照 */

    if (!s_stats_mux) { free(snap); return; }
    TaskStatus_t *old = NULL;
    xSemaphoreTake(s_stats_mux, portMAX_DELAY);
    old = s_stats_prev;                                /* 换出新快照 */
    s_stats_prev       = snap;
    s_stats_prev_count = n;
    s_stats_prev_total = total;
    s_stats_prev_tick  = xTaskGetTickCount();          /* 记录采样时刻 */
    xSemaphoreGive(s_stats_mux);
    if (old) free(old);                                /* 锁外释放 */
}

/* stats 命令: 打印两次采样窗口内各任务 CPU 占用百分比 */
static void cmd_stats(void)
{
    if (!s_stats_mux) {
        printf("[stats] 统计未就绪\n");
        return;
    }

    /* 锁内深拷贝上次快照: 之后不再引用 s_stats_prev, 解锁后由本函数独占副本 */
    xSemaphoreTake(s_stats_mux, portMAX_DELAY);
    UBaseType_t prev_count = s_stats_prev_count;
    uint32_t    prev_total = s_stats_prev_total;
    TickType_t  prev_tick  = s_stats_prev_tick;
    TaskStatus_t *prev = NULL;
    if (s_stats_prev && prev_count) {
        prev = malloc(prev_count * sizeof(TaskStatus_t));
        if (prev) memcpy(prev, s_stats_prev, prev_count * sizeof(TaskStatus_t));
    }
    xSemaphoreGive(s_stats_mux);

    if (!prev) {
        printf("[stats] 暂无数据，请等待下一次采样\n");
        return;
    }

    UBaseType_t n = uxTaskGetNumberOfTasks();
    TaskStatus_t *cur = malloc(n * sizeof(TaskStatus_t));   /* 当前快照 */
    if (!cur) {
        printf("[stats] 内存不足\n");
        free(prev);
        return;
    }

    uint32_t cur_total = 0;
    n = uxTaskGetSystemState(cur, n, &cur_total);

    TickType_t now = xTaskGetTickCount();
    uint32_t delta_ms = (now - prev_tick) * portTICK_PERIOD_MS;   /* 两次采样间隔 (ms) */
    uint32_t delta_total = cur_total - prev_total;                /* 窗口内总运行计数增量 */

    if (delta_total == 0) {
        printf("[stats] 测不到有效差值, 请稍后再试\n");
        free(cur);
        free(prev);
        return;
    }

    printf("\n===== CPU 占用 (~%"PRIu32"ms 窗口) =====\n", delta_ms);
    printf("%-20s %6s  %4s  %s\n", "任务名", "CPU%", "Prio", "Core");

    /* 对每个当前任务, 用句柄在两次快照间配对, 计算其运行计数增量占比 */
    for (UBaseType_t i = 0; i < n; i++) {
        uint32_t delta_task = 0;
        for (UBaseType_t j = 0; j < prev_count; j++) {
            if (cur[i].xHandle == prev[j].xHandle) {   /* 同一任务 (按句柄匹配) */
                if (cur[i].ulRunTimeCounter > prev[j].ulRunTimeCounter) {
                    delta_task = cur[i].ulRunTimeCounter -
                                 prev[j].ulRunTimeCounter;
                }
                break;
            }
        }
        float pct = (float)delta_task / (float)delta_total * 100.0f;
        printf("%-20s %5.1f%%  %4u  %d\n",
               cur[i].pcTaskName, pct, (unsigned)cur[i].uxCurrentPriority, cur[i].xCoreID);
    }
    printf("===============================\n");

    free(cur);
    free(prev);
}

/* ram 命令: 打印内部 RAM 空闲情况 */
static void cmd_free(void)
{
    uint32_t total = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);          /* 内部 RAM 总空闲 */
    uint32_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL); /* 最大连续块 */
    printf("[RAM] 总空闲: %"PRIu32"KB  最大连续块: %"PRIu32"KB\n",
           total / 1024, largest / 1024);
}

/* psram 命令: 打印外部 PSRAM 空闲情况 */
static void cmd_psram(void)
{
    if (!esp_psram_is_initialized()) {
        printf("[PSRAM] 未启用\n");
        return;
    }
    uint32_t total = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);            /* PSRAM 总空闲 */
    uint32_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM); /* 最大连续块 */
    printf("[PSRAM] 总空闲: %"PRIu32"KB  最大连续块: %"PRIu32"KB\n",
           total / 1024, largest / 1024);
}

/* 打印指定 RTOS 任务的 backtrace (带任务名): name=任务名.
 * 阻塞/挂起任务: 用 TCB 保存的栈顶 pxTopOfStack (pc/a1/a0) 回溯其自身栈, 精确.
 * 正在运行的任务: 快照为最近一次被切出时的上下文 (略旧, 但栈内容仍在).
 * 本任务(自身): 用实时上下文. */
static void cmd_backtrace(const char *name)
{
    TaskHandle_t h = xTaskGetHandle(name);              /* 按名字查任务句柄 */
    if (!h) {
        printf("[bt] 未找到任务: %s\n", name);
        return;
    }

    esp_backtrace_frame_t fr = {0};                     /* 回溯起点帧 (pc/sp/next_pc) */

    if (h == xTaskGetCurrentTaskHandle()) {             /* 要打印的就是控制台任务自己 */
        esp_backtrace_get_start(&fr.pc, &fr.sp, &fr.next_pc);   /* 取实时寄存器上下文 */
        printf("[bt] %s (current task)\n", name);
        esp_backtrace_print_from_frame(50, &fr, false);         /* 打印 50 帧 */
        return;
    }

    /* 其他任务: 从任务自身栈回溯, 必须冻结调度保证 TCB 一致 */
    TaskSnapshot_t snap;
    vTaskSuspendAll();                       /* 冻结调度, 与 esp_backtrace_print_all_tasks 一致 */
    BaseType_t ok = vTaskGetSnapshot(h, &snap);
    xTaskResumeAll();

    if (ok != pdTRUE) {
        printf("[bt] 任务快照失败: %s\n", name);
        return;
    }

    /* Xtensa 异常帧就在任务栈顶: 取 pc(返回地址)/a1(栈指针)/a0(链接寄存器) 重建回溯 */
    XtExcFrame *f = (XtExcFrame *)snap.pxTopOfStack;
    fr.pc = f->pc;
    fr.sp = f->a1;
    fr.next_pc = f->a0;
    printf("[bt] %s (saved stack)\n", name);
    esp_backtrace_print_from_frame(50, &fr, false);
}

/* bt 命令 (无参数): 打印所有任务 backtrace */
static void cmd_backtrace_all(void)
{
    printf("[bt] 所有任务 backtrace:\n");
    esp_backtrace_print_all_tasks(50);
}

/* 控制台任务: 非阻塞读串口, 逐字符拼行, 回车后解析执行 */
static void console_task(void *arg)
{
    fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);   /* 设非阻塞读, 任务循环里轮询 */

    printf("\n=== 音乐播放器 ===\n");
    printf("系统命令: stats | ram | psram | vbat | temp | bt [任务名]\n");
    printf("应用命令: scan | conn <名称> | disconn | play | stop | pause | info\n");
    //printf("命令> ");

    char line[LINE_BUF_SIZE];   /* 当前行缓冲 */
    int  line_pos = 0;          /* 已输入字符数 */

    while (1) {
        char ch;
        while (read(STDIN_FILENO, &ch, 1) > 0) {   /* 循环读直到没有新字符 */
            if (ch == '\n' || ch == '\r') {        /* 回车: 整行命令就绪 */
                if (line_pos > 0) {
                    line[line_pos] = '\0';

                    /* ── 系统命令 (本地执行, 不进队列) ── */
                    if (strcmp(line, "stats") == 0) {
                        cmd_stats();
                    } else if (strcmp(line, "ram") == 0) {
                        cmd_free();
                    } else if (strcmp(line, "vbat") == 0) {
                        float v = atomic_load_float(&g_vbat);   /* 读采样任务更新的电池电压 */
                        printf("[电池] %.2f V\n", v);
                    } else if (strcmp(line, "temp") == 0) {
                        float t = atomic_load_float(&g_cpu_temp);   /* 读 CPU 温度 */
                        printf("[CPU温度] %.1f C\n", t);
                    } else if (strcmp(line, "psram") == 0) {
                        cmd_psram();
                    } else if (strcmp(line, "bt") == 0) {
                        cmd_backtrace_all();
                    } else if (strncmp(line, "bt ", 3) == 0) {   /* "bt <任务名>" */
                        cmd_backtrace(line + 3);
                    } else {
                        /* ── 应用命令 (封包后发到 app 队列, 由 UI 消费) ── */
                        app_cmd_t cmd;
                        memset(&cmd, 0, sizeof(cmd));   /* 清零, 保证 param 全部 '\0' */

                        if (strcmp(line, "scan") == 0) {
                            cmd.type = APP_CMD_BT_SCAN;
                        } else if (strncmp(line, "conn ", 5) == 0) {   /* "conn <设备名>" */
                            cmd.type = APP_CMD_BT_CONNECT;
                            /* 截取设备名到 param, 用 strnlen+memcpy 避免越界 */
                            size_t plen = strnlen(line + 5, sizeof(cmd.param) - 1);
                            memcpy(cmd.param, line + 5, plen);
                            cmd.param[plen] = '\0';
                        } else if (strcmp(line, "disconn") == 0) {
                            cmd.type = APP_CMD_BT_DISCONNECT;
                        } else if (strcmp(line, "play") == 0) {
                            cmd.type = APP_CMD_PLAY;
                        } else if (strcmp(line, "stop") == 0) {
                            cmd.type = APP_CMD_STOP;
                        } else if (strcmp(line, "pause") == 0) {
                            cmd.type = APP_CMD_PAUSE;
                        } else if (strcmp(line, "info") == 0) {
                            cmd.type = APP_CMD_INFO;
                        } else {
                            printf("未知命令: %s\n", line);
                            line_pos = 0;
                            //printf("命令> ");
                            continue;
                        }

                        xQueueSend(s_app_cmd_queue, &cmd, 0);   /* 投递, 非阻塞 */
                    }

                    line_pos = 0;
                    //printf("命令> ");
                }
            } else if (ch == '\b' || ch == 127) {   /* 退格: 回退一个字符 */
                if (line_pos > 0) line_pos--;
            } else if (line_pos < LINE_BUF_SIZE - 1) {   /* 普通字符: 追加到行缓冲 */
                line[line_pos++] = ch;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));   /* 轮询间隔 50ms, 避免空转占满 CPU */
    }
}

/* 控制台初始化: app_cmd_queue=解析出的应用命令要投递到的队列.
 * 创建 1s 周期 stats 采样定时器 + 控制台任务 (固定 core 1). */
void console_init(QueueHandle_t app_cmd_queue)
{
    s_app_cmd_queue = app_cmd_queue;

    s_stats_mux = xSemaphoreCreateMutex();   /* 统计快照互斥量 (须在定时器启动前建好) */

    TimerHandle_t timer = xTimerCreate("stats", pdMS_TO_TICKS(1000),
                                       pdTRUE, NULL, stats_timer_cb);   /* 自动重载定时器 */
    xTimerStart(timer, 0);

    xTaskCreatePinnedToCore(console_task, "sys_serial", 2048, NULL, 1, NULL, 1);
}
