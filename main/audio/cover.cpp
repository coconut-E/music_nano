#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "JPEGDEC.h"
#include "cover.h"
#include "app.h"

/* 该组件只编译了 C API (JPEG_openRAM 等), 在此声明供 C++ 使用 */
extern "C" {
int JPEG_openRAM(JPEGIMAGE *pJPEG, uint8_t *pData, int iDataSize, JPEG_DRAW_CALLBACK *pfnDraw);
int JPEG_decode(JPEGIMAGE *pJPEG, int x, int y, int iOptions);
void JPEG_close(JPEGIMAGE *pJPEG);
void JPEG_setPixelType(JPEGIMAGE *pJPEG, int iType);
int JPEG_getWidth(JPEGIMAGE *pJPEG);
int JPEG_getHeight(JPEGIMAGE *pJPEG);
int JPEG_getLastError(JPEGIMAGE *pJPEG);
}

/* JPEG 错误码 → 字符串 (见 JPEGDEC.h 错误枚举) */
static const char *jpeg_err_str(int err)
{
    switch (err) {
    case 0: return "OK";
    case 1: return "INVALID_PARAMETER";
    case 2: return "DECODE_ERROR";
    case 3: return "UNSUPPORTED_FEATURE";
    case 4: return "INVALID_FILE";
    default: return "UNKNOWN";
    }
}

#define COVER_TAG         "COVER"
#define COVER_SIZE        100            /* 封面输出尺寸 100x100 */
#define COVER_PIXELS      (COVER_SIZE * COVER_SIZE)
#define COVER_STACK_WORDS 4096           /* 解码任务栈 (字) */

/* LVGL 开了 LV_COLOR_16_SWAP, 最终输出缓冲需字节交换 */

static QueueHandle_t     s_app_cmd_queue = NULL;   /* 完成通知队列 (发 APP_CMD_COVER_READY) */
static TaskHandle_t      s_task = NULL;            /* 解码任务句柄 */
static volatile bool     s_task_ready = false;     /* 解码任务已进入主循环 (可安全挂起) */
static StaticTask_t     *s_tcb = NULL;             /* 静态任务 TCB (必须放内部 RAM) */
static StackType_t      *s_stack = NULL;           /* 静态任务栈 (PSRAM) */
static SemaphoreHandle_t s_job_sem = NULL;         /* 作业信号量 (计数=待处理作业数) */
static SemaphoreHandle_t s_slot_mutex = NULL;      /* 保护 s_job_data/s_job_size */

/* 待解码的封面源数据 (PSRAM, 所有权归封面模块, 解码后释放) */
static uint8_t *s_job_data = NULL;
static size_t   s_job_size = 0;

static uint16_t *s_out_buf = NULL;  /* 常驻 100x100 RGB565 输出 (PSRAM) */
static JPEGIMAGE *s_jpeg = NULL;    /* 常驻解码器 (PSRAM) */

static uint16_t *s_raw = NULL;      /* 解码中间缓冲 (PSRAM, 临时) */
static int       s_raw_w = 0;       /* 中间缓冲宽 */
static int       s_raw_h = 0;       /* 中间缓冲高 */

/* JPEG 解码回调: 把 MCU 块拷进中间缓冲 (右边缘用 iWidthUsed, 并钳制到缓冲边界).
 * pDraw=解码器给的 MCU 块信息. 返回非 0 继续解码. */
static int cover_jpeg_draw(JPEGDRAW *pDraw)
{
    if (!s_raw) return 0;

    /* JPEGDEC 提供的块: 拷贝宽度取 iWidthUsed (右边缘被裁掉多余列),
     * 但源数据每行跨度是整块宽 iWidth (JPEGDEC 内部 iPitch == iWidth) */
    int w = pDraw->iWidthUsed;
    int h = pDraw->iHeight;
    int x = pDraw->x;
    int y = pDraw->y;
    if (w <= 0 || h <= 0) return 1;

    /* 防御性钳制: 任何越界都截断, 杜绝写穿 s_raw */
    if (x >= s_raw_w || y >= s_raw_h) return 1;
    if (x + w > s_raw_w) w = s_raw_w - x;
    if (y + h > s_raw_h) h = s_raw_h - y;
    if (w <= 0 || h <= 0) return 1;

    uint16_t *dest = s_raw + (size_t)y * s_raw_w + x;   /* 目标位置 */
    const uint16_t *src = pDraw->pPixels;
    while (h-- > 0) {   /* 逐行拷贝 */
        memcpy(dest, src, (size_t)w * sizeof(uint16_t));
        dest += s_raw_w;                /* 目标行宽 = s_raw_w */
        src  += (size_t)pDraw->iWidth;  /* 源行跨度 = 整块宽 (不是 iWidthUsed) */
    }
    return 1;
}

/* 面积平均采样 (缩小时): 求矩形区域内所有像素的 RGB 平均值.
 * src=源缓冲, src_w=源宽, x0/y0/x1/y1=采样矩形. 返回平均后 RGB565. */
static uint16_t cover_avg_box(const uint16_t *src, int src_w, int x0, int x1,
                              int y0, int y1)
{
    uint32_t r = 0, g = 0, b = 0, cnt = 0;
    for (int y = y0; y <= y1; y++) {
        const uint16_t *row = src + (size_t)y * src_w;
        for (int x = x0; x <= x1; x++) {
            uint16_t p = row[x];
            r += (p >> 11) & 0x1F;   /* RGB565 拆出 R 通道 */
            g += (p >> 5)  & 0x3F;   /* G 通道 */
            b += p & 0x1F;           /* B 通道 */
            cnt++;
        }
    }
    r = (r + cnt / 2) / cnt;   /* 四舍五入平均 */
    g = (g + cnt / 2) / cnt;
    b = (b + cnt / 2) / cnt;
    return (uint16_t)((r << 11) | (g << 5) | b);   /* 重新拼回 RGB565 */
}

/* 居中裁方 + 缩放到 100x100 (标准 RGB565):
 * 取源图中心的方形区域, 再按需缩小(面积平均)/放大(最近邻). */
static void cover_scale_crop_to_100(const uint16_t *src, int src_w, int src_h)
{
    int side = src_w < src_h ? src_w : src_h;   /* 方形边长 = 短边 */
    int cx = (src_w - side) / 2;   /* 中心方区域左上角 x */
    int cy = (src_h - side) / 2;   /* 中心方区域左上角 y */

    for (int ty = 0; ty < COVER_SIZE; ty++) {
        uint16_t *dst = s_out_buf + (size_t)ty * COVER_SIZE;
        if (side >= COVER_SIZE) {
            /* 缩小: 每个输出像素对应源区域取平均 */
            int y0 = cy + (ty * side) / COVER_SIZE;
            int y1 = cy + ((ty + 1) * side) / COVER_SIZE - 1;
            if (y1 < y0) y1 = y0;
            for (int tx = 0; tx < COVER_SIZE; tx++) {
                int x0 = cx + (tx * side) / COVER_SIZE;
                int x1 = cx + ((tx + 1) * side) / COVER_SIZE - 1;
                if (x1 < x0) x1 = x0;
                dst[tx] = cover_avg_box(src, src_w, x0, x1, y0, y1);
            }
        } else {
            /* 放大: 最近邻采样 */
            int sy = cy + (ty * side) / COVER_SIZE;
            for (int tx = 0; tx < COVER_SIZE; tx++) {
                int sx = cx + (tx * side) / COVER_SIZE;
                dst[tx] = src[(size_t)sy * src_w + sx];
            }
        }
    }
}

/* LV_COLOR_16_SWAP: 标准 RGB565 字节交换 (高低字节互换) */
static void cover_swap_rgb565(uint16_t *buf, int n)
{
    for (int i = 0; i < n; i++) {
        buf[i] = (uint16_t)((buf[i] << 8) | (buf[i] >> 8));
    }
}

/* 用指定缩放选项解码到中间缓冲 s_raw (后续 crop/缩放写入 s_out_buf).
 * 成功返回 1, 失败返回 0. opt=JPEG 缩放选项, w/h=原图尺寸 */
static int cover_decode_attempt(int opt, int w, int h)
{
    int rw = w, rh = h;   /* 解码目标尺寸 (缩放后) */
    if (opt == JPEG_SCALE_EIGHTH)       { rw = w / 8; rh = h / 8; }
    else if (opt == JPEG_SCALE_QUARTER) { rw = w / 4; rh = h / 4; }
    else if (opt == JPEG_SCALE_HALF)    { rw = w / 2; rh = h / 2; }

    /* 中间缓冲留 16 行列余量 (JPEGDEC 按 MCU 块写, 边缘可能越界); 
     * 回调内已按 s_raw_w/h 钳制, 真正防越界靠钳制 */
    size_t raw_bytes = (size_t)(rw + 16) * (rh + 16) * sizeof(uint16_t);
    uint16_t *raw = (uint16_t *)heap_caps_malloc(raw_bytes, MALLOC_CAP_SPIRAM);
    if (!raw) {
        ESP_LOGW(COVER_TAG, "中间缓冲分配失败 (%u KB)", (unsigned)(raw_bytes / 1024));
        JPEG_close(s_jpeg);
        return 0;
    }
    s_raw = raw;      /* 供解码回调写入 */
    s_raw_w = rw;
    s_raw_h = rh;

    JPEG_setPixelType(s_jpeg, RGB565_LITTLE_ENDIAN);   /* 输出 RGB565 小端 */
    uint8_t mode = s_jpeg->ucMode;    /* 记录编码模式 (诊断) */
    uint8_t comp = s_jpeg->ucNumComponents;
    int rc = JPEG_decode(s_jpeg, 0, 0, opt);   /* 执行解码 */
    int err = JPEG_getLastError(s_jpeg);
    JPEG_close(s_jpeg);
    s_raw = NULL;

    if (rc != 1) {   /* 解码失败 */
        ESP_LOGW(COVER_TAG, "解码失败 rc=%d err=%d (%s) %dx%d mode=0x%02x comp=%u opt=%d",
                 rc, err, jpeg_err_str(err), w, h, mode, comp, opt);
        heap_caps_free(raw);
        return 0;
    }

    cover_scale_crop_to_100(raw, rw, rh);   /* 缩放居中裁方到 100x100 */
    heap_caps_free(raw);
    return 1;
}

/* 通知 UI 封面结果: ok=true 传 s_out_buf, ok=false 传 NULL (UI 显示默认图标) */
static void cover_send_result(bool ok)
{
    if (!s_app_cmd_queue) return;
    app_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = APP_CMD_COVER_READY;
    void *buf = ok ? (void *)s_out_buf : NULL;   /* 传指针 (指针按位拷贝进 param) */
    memcpy(cmd.param, &buf, sizeof(buf));
    xQueueSend(s_app_cmd_queue, &cmd, 0);
}

/* 解码一份封面数据: data=JPEG数据, size=大小. 结果通过命令队列通知 UI. */
static void cover_decode_job(const uint8_t *data, size_t size)
{
    if (!s_out_buf || !s_jpeg || !data || size == 0) {
        cover_send_result(false);
        return;
    }

    int rc_open = JPEG_openRAM(s_jpeg, (uint8_t *)data, (int)size, cover_jpeg_draw);   /* 从内存打开 JPEG */
    if (rc_open != 1) {
        int err = JPEG_getLastError(s_jpeg);
        ESP_LOGW(COVER_TAG, "openRAM 失败 rc=%d err=%d (%s)", rc_open, err, jpeg_err_str(err));
        cover_send_result(false);
        return;
    }

    /* 渐进式 JPEG (SOF2, 0xc2) 不支持, 跳过 */
    if (s_jpeg->ucMode == 0xc2) {
        ESP_LOGW(COVER_TAG, "渐进式 JPEG, 跳过封面");
        JPEG_close(s_jpeg);
        cover_send_result(false);
        return;
    }

    int w = JPEG_getWidth(s_jpeg);
    int h = JPEG_getHeight(s_jpeg);
    if (w <= 0 || h <= 0) {   /* 尺寸无效 */
        JPEG_close(s_jpeg);
        cover_send_result(false);
        return;
    }
    ESP_LOGI(COVER_TAG, "封面 %dx%d mode=0x%02x comp=%u", w, h,
             s_jpeg->ucMode, s_jpeg->ucNumComponents);

    /* 按尺寸选择缩放档: 尽量用最低解码分辨率省内存/时间 */
    int opt = 0;
    if (w / 8 >= COVER_SIZE && h / 8 >= COVER_SIZE) {
        opt = JPEG_SCALE_EIGHTH;
    } else if (w / 4 >= COVER_SIZE && h / 4 >= COVER_SIZE) {
        opt = JPEG_SCALE_QUARTER;
    } else if (w / 2 >= COVER_SIZE && h / 2 >= COVER_SIZE) {
        opt = JPEG_SCALE_HALF;
    }

    int ok = cover_decode_attempt(opt, w, h);
    if (!ok && opt != 0) {
        /* 缩放解码失败 → 回退全量解码, 由 cover_scale_crop_to_100 负责缩放 */
        ESP_LOGW(COVER_TAG, "缩放解码失败, 回退全量解码");
        rc_open = JPEG_openRAM(s_jpeg, (uint8_t *)data, (int)size, cover_jpeg_draw);
        if (rc_open != 1) {
            int err = JPEG_getLastError(s_jpeg);
            ESP_LOGW(COVER_TAG, "重开 openRAM 失败 rc=%d err=%d (%s)", rc_open, err, jpeg_err_str(err));
            cover_send_result(false);
            return;
        }
        if (s_jpeg->ucMode == 0xc2) {   /* 重开后仍是渐进式 */
            JPEG_close(s_jpeg);
            cover_send_result(false);
            return;
        }
        ok = cover_decode_attempt(0, w, h);   /* 全尺寸解码 */
    }
    if (!ok) {
        cover_send_result(false);
        return;
    }

    cover_swap_rgb565(s_out_buf, COVER_PIXELS);   /* 适配 LVGL 字节序 */
    cover_send_result(true);
}

/* 封面解码任务: 等作业信号量, 取数据解码后释放 */
static void cover_task(void *arg)
{
    s_task_ready = true;   /* 已进入主循环: 允许模式切换时安全挂起 */

    for (;;) {
        if (!xSemaphoreTake(s_job_sem, portMAX_DELAY)) continue;   /* 阻塞等作业 */

        /* 取走作业 (互斥保护) */
        uint8_t *data;
        size_t size;
        xSemaphoreTake(s_slot_mutex, portMAX_DELAY);
        data = s_job_data;
        size = s_job_size;
        s_job_data = NULL;
        s_job_size = 0;
        xSemaphoreGive(s_slot_mutex);

        if (data && size > 0) {
            cover_decode_job(data, size);   /* 解码并通知 UI */
            heap_caps_free(data);           /* 释放作业数据 */
        }
    }
}

/* 启动封面解码任务: app_cmd_queue=完成通知要投递的命令队列 */
void cover_init(QueueHandle_t app_cmd_queue)
{
    s_app_cmd_queue = app_cmd_queue;
    s_job_sem = xSemaphoreCreateCounting(4, 0);   /* 计数信号量, 最多积压 4 个作业 */
    s_slot_mutex = xSemaphoreCreateMutex();

    /* 常驻缓冲尽量放 PSRAM 省内部 RAM (TCB 例外, 必须内部 RAM) */
    s_out_buf = (uint16_t *)heap_caps_malloc(COVER_PIXELS * sizeof(uint16_t),
                                             MALLOC_CAP_SPIRAM);
    s_jpeg = (JPEGIMAGE *)heap_caps_malloc(sizeof(JPEGIMAGE), MALLOC_CAP_SPIRAM);
    s_stack = (StackType_t *)heap_caps_malloc(COVER_STACK_WORDS * sizeof(StackType_t),
                                              MALLOC_CAP_SPIRAM);
    s_tcb = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t),
                                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if (!s_out_buf || !s_jpeg || !s_stack || !s_tcb) {
        ESP_LOGE(COVER_TAG, "PSRAM 分配失败, 封面功能禁用");
        return;
    }

    /* 静态创建: 栈在 PSRAM, 固定 core 1 */
    s_task = xTaskCreateStaticPinnedToCore(cover_task, "cover", COVER_STACK_WORDS,
                                           NULL, 1, s_stack, s_tcb, 1);
    ESP_LOGI(COVER_TAG, "封面解码任务已创建 (PSRAM 栈)");
}

/* 提交封面解码作业: jpg=JPEG数据 (所有权移交封面模块), size=数据大小.
 * 返回 true=已接管; false=因未就绪/参数非法未接管, 调用方仍持有数据需自行释放 */
bool cover_submit_job(const uint8_t *jpg, size_t size)
{
    if (!jpg || size == 0 || !s_task || !s_job_sem || !s_slot_mutex) return false;

    xSemaphoreTake(s_slot_mutex, portMAX_DELAY);
    if (s_job_data) {
        heap_caps_free(s_job_data);  /* 丢弃未取走的旧 job */
    }
    s_job_data = (uint8_t *)jpg;
    s_job_size = size;
    xSemaphoreGive(s_slot_mutex);

    xSemaphoreGive(s_job_sem);   /* 唤醒解码任务 */
    return true;
}

/* 通知 UI: 当前歌曲无内嵌封面, 回退默认图标 */
void cover_notify_no_cover(void)
{
    cover_send_result(false);
}

/* 挂起封面解码任务 (模式切到小说): 等任务就绪后再挂 */
extern "C" void cover_task_pause(void)
{
    if (!s_task) return;
    for (int i = 0; i < 200 && !s_task_ready; i++) {   /* 最多等 ~1s */
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (!s_task_ready) return;
    vTaskSuspend(s_task);
}

/* 恢复封面解码任务 (模式切回音乐) */
extern "C" void cover_task_resume(void)
{
    if (s_task) vTaskResume(s_task);
}
