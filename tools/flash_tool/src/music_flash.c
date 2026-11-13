/*
 * music_nano 烧录工具  (Windows x86-64 / MinGW-w64)
 *
 * 只服务本仓库这套固件 (分区布局见仓库根 partitions.csv):
 *   bootloader@0x1000 / 分区表@0x8000 / ota_0@0x30000 / ota_1@0x310000 / font@0x5F0000
 *
 * 定位: 只做两件事
 *   (1) 芯片全空时的首次全量烧录 (bootloader + 分区表 + ota_0 + 字库)
 *   (2) 测试用的镜像抓取
 * 日常升级走网络 OTA (app 内部下载并写 app 分区), 不经过本工具。
 * 串口协议按 esptool 4.12 的 ESP32 ROM + flasher stub 行为实现:
 *   ROM 115200 -> 上传 stub -> 切换波特率 -> 直写 flash(不压缩) -> MD5 回读校验 -> 复位
 *
 * 注意: 烧录数据一律走 ESP_FLASH_DATA(原始数据), 不使用 ESP_FLASH_DEFL_*(压缩)。
 *
 * 内嵌的 ESP32 flasher stub 来自 esptool (GPL-2.0-or-later), 见 src/stub_esp32.h。
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <conio.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "md5.h"
#include "stub_esp32.h"

/* ------------------------------------------------------------------ */
/* 常量                                                                */
/* ------------------------------------------------------------------ */

#define APP_TITLE "music_nano 烧录工具"
#define APP_VER   "1.0"

#define ESP_SYNC            0x08
#define ESP_READ_REG        0x0A
#define ESP_MEM_BEGIN       0x05
#define ESP_MEM_DATA        0x07
#define ESP_MEM_END         0x06
#define ESP_FLASH_BEGIN     0x02
#define ESP_FLASH_DATA      0x03
#define ESP_FLASH_END       0x04
#define ESP_SPI_ATTACH      0x0D
#define ESP_CHANGE_BAUDRATE 0x0F
#define ESP_SPI_FLASH_MD5   0x13
#define ESP_ERASE_FLASH     0xD0
#define ESP_ERASE_REGION    0xD1
#define ESP_READ_FLASH_SLOW 0x0E

#define ESP32_MAGIC         0x00F01D83u
#define ESP32_MAGIC_ADDR    0x40001000u
#define ESP32_EFUSE_BASE    0x3FF5A000u

#define PT_ADDR             0x8000u
#define PT_LEN              0x0C00u

/* 固定布局 (partitions.csv). 这几个不是"默认值"而是硬约束:
 * 这个工程只有这一套合法布局, 写错地址会直接写成砖。 */
#define APP0_ADDR           0x030000u   /* ota_0 */
#define APP1_ADDR           0x310000u   /* ota_1 */
#define FONT_ADDR           0x5F0000u   /* font (字库) */
/* 系统区上界: 0x030000 以下只有 bootloader/分区表/nvs/phy_init/otadata,
 * 没有任何空隙可以写别的东西。旧布局这里还有 0x22000-0x40000 的 64KB 对齐空洞,
 * nvs 扩到 144KB 之后空洞消失, 判定规则反而变简单了。 */
#define SYS_END             0x030000u
#define FLASH_END_ADDR      0x800000u   /* 8MB */

#define STUB_RAM_BLOCK      0x1800
#define FLASH_BLOCK_STUB    0x4000
#define FLASH_BLOCK_ROM     0x0400

#define COL_DEF  7
#define COL_OK   10
#define COL_WARN 14
#define COL_ERR  12
#define COL_INFO 11
#define COL_DIM  8

#define MAX_PARTS 8

/* ------------------------------------------------------------------ */
/* 全局                                                                */
/* ------------------------------------------------------------------ */

static int  g_nopause;
static int  g_force_erase;
static int  g_no_erase;
static int  g_trace;
static int  g_selftest;
static int  g_probe_only;
static char g_port_opt[32];
static int  g_baud = 921600;
static int  g_erase_region_set;
static uint32_t g_erase_addr;
static uint32_t g_erase_len;

static HANDLE g_hout;
static char   g_err[256];

static wchar_t g_exe_dir[MAX_PATH];
static wchar_t g_root[MAX_PATH];

typedef struct {
    uint32_t addr;
    wchar_t  path[MAX_PATH];
    char     label[80];
    uint8_t *data;
    DWORD    size;
} Part;

static Part g_parts[MAX_PARTS];
static int  g_nparts;

/* ------------------------------------------------------------------ */
/* 控制台 / 输出                                                       */
/* ------------------------------------------------------------------ */

static void col(int c) { SetConsoleTextAttribute(g_hout, (WORD)c); }

static void con_init(void)
{
    g_hout = GetStdHandle(STD_OUTPUT_HANDLE);
    if (g_hout == INVALID_HANDLE_VALUE) {
        g_hout = NULL;
    }
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
    setvbuf(stdout, NULL, _IONBF, 0);
}

static void tag_line(int color, const char *tag, const char *fmt, ...)
{
    va_list ap;
    char buf[1024];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (g_hout) {
        col(color);
        printf("%s", tag);
        col(COL_DEF);
    }
    printf("%s\n", buf);
}

#define SAY(...)     printf(__VA_ARGS__)
#define OK_LINE(...) tag_line(COL_OK, "[OK] ", __VA_ARGS__)
#define WARN_LINE(...) tag_line(COL_WARN, "[!!] ", __VA_ARGS__)
#define ERR_LINE(...)  tag_line(COL_ERR, "[xx] ", __VA_ARGS__)
#define INFO_LINE(...) tag_line(COL_INFO, "[..] ", __VA_ARGS__)

static void pause_exit(void)
{
    if (g_nopause) {
        return;
    }
    if (g_hout) {
        col(COL_DIM);
    }
    printf("\n按任意键结束...");
    fflush(stdout);
    if (g_hout) {
        col(COL_DEF);
    }
    _getch();
    printf("\n");
}

static void usage(void)
{
    SAY("\n用法:\n");
    SAY("  music_flash.exe                 自动检测串口并烧录\n");
    SAY("  music_flash.exe /port COM5      指定串口\n");
    SAY("  music_flash.exe /baud 460800    指定波特率 (默认 921600)\n");
    SAY("  music_flash.exe /erase          强制整片擦除后再烧录\n");
    SAY("  music_flash.exe /noerase        即使分区表不同也直写(不清设置)\n");
    SAY("  music_flash.exe /nopause        结束不等待按键\n");
    SAY("  music_flash.exe /trace          打印收发报文(调试用)\n");
    SAY("  music_flash.exe /probe          只连接并显示芯片信息, 不烧录\n");
    SAY("  music_flash.exe /erase-region 0x7F0000 0x10000   调试: 只擦除一段\n");
    SAY("  music_flash.exe /selftest       协议自检(不接触设备)\n");
}

/* ------------------------------------------------------------------ */
/* 宽字符路径 / 文件                                                   */
/* ------------------------------------------------------------------ */

static void to_w(const char *u8, wchar_t *w, size_t cap)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, u8, -1, w, (int)cap);
    if (n <= 0) {
        w[0] = 0;
    }
}

static void to_u8(const wchar_t *w, char *u8, size_t cap)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, u8, (int)cap, NULL, NULL);
    if (n <= 0) {
        u8[0] = 0;
    }
}

static int fexists(const wchar_t *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static void wjoin(wchar_t *dst, size_t cap, const wchar_t *dir, const wchar_t *name)
{
    if (wcslen(dir) + wcslen(name) + 2 >= cap) {
        dst[0] = 0;
        return;
    }
    wcscpy(dst, dir);
    wcscat(dst, L"\\");
    wcscat(dst, name);
}

static uint8_t *read_file(const wchar_t *path, DWORD *out_len)
{
    HANDLE h;
    LARGE_INTEGER sz;
    uint8_t *buf;
    DWORD got = 0;

    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return NULL;
    }
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (64 << 20)) {
        CloseHandle(h);
        return NULL;
    }
    buf = (uint8_t *)malloc((size_t)sz.QuadPart + 16);
    if (!buf) {
        CloseHandle(h);
        return NULL;
    }
    if (!ReadFile(h, buf, (DWORD)sz.QuadPart, &got, NULL) || got != (DWORD)sz.QuadPart) {
        free(buf);
        CloseHandle(h);
        return NULL;
    }
    CloseHandle(h);
    *out_len = got;
    return buf;
}

static int parent_dir(const wchar_t *in, wchar_t *out)
{
    wchar_t *p;
    wcscpy(out, in);
    p = wcsrchr(out, L'\\');
    if (!p) {
        return 0;
    }
    if (p == out) {
        p[1] = 0;
    } else {
        *p = 0;
    }
    return 1;
}

static int has_fw(const wchar_t *dir)
{
    wchar_t t[MAX_PATH];
    wjoin(t, MAX_PATH, dir, L"flash_args");
    if (fexists(t)) {
        return 1;
    }
    wjoin(t, MAX_PATH, dir, L"music_nano.bin");
    if (fexists(t)) {
        return 1;
    }
    wjoin(t, MAX_PATH, dir, L"bootloader");
    {
        wchar_t t2[MAX_PATH];
        wjoin(t2, MAX_PATH, t, L"bootloader.bin");
        if (fexists(t2)) {
            return 1;
        }
    }
    return 0;
}

static void scan_for_fw(const wchar_t *dir, int depth)
{
    wchar_t pat[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;

    if (g_root[0]) {
        return;
    }
    if (has_fw(dir)) {
        wcsncpy(g_root, dir, MAX_PATH - 1);
        g_root[MAX_PATH - 1] = 0;
        return;
    }
    if (depth <= 0) {
        return;
    }
    wjoin(pat, MAX_PATH, dir, L"*");
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) {
            wchar_t sub[MAX_PATH];
            wjoin(sub, MAX_PATH, dir, fd.cFileName);
            scan_for_fw(sub, depth - 1);
            if (g_root[0]) {
                break;
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static int find_root(void)
{
    wchar_t exe[MAX_PATH];
    wchar_t *p;
    wchar_t up1[MAX_PATH], up2[MAX_PATH];

    g_root[0] = 0;
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wcsncpy(g_exe_dir, exe, MAX_PATH - 1);
    g_exe_dir[MAX_PATH - 1] = 0;
    p = wcsrchr(g_exe_dir, L'\\');
    if (p) {
        *p = 0;
    }

    scan_for_fw(g_exe_dir, 1);
    if (!g_root[0] && parent_dir(g_exe_dir, up1)) {
        scan_for_fw(up1, 1);
        if (!g_root[0] && parent_dir(up1, up2)) {
            scan_for_fw(up2, 1);
        }
    }
    return g_root[0] ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* flash_args / 固件清单                                               */
/* ------------------------------------------------------------------ */

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static const char *base_of(const char *path)
{
    const char *b = path;
    for (const char *s = path; *s; s++) {
        if (*s == '/' || *s == '\\') {
            b = s + 1;
        }
    }
    return b;
}

static int add_part(uint32_t addr, const char *name)
{
    Part *pt;
    wchar_t wname[MAX_PATH];
    wchar_t full[MAX_PATH];
    const char *base;
    uint8_t *data;
    DWORD len = 0;

    if (g_nparts >= MAX_PARTS) {
        return -1;
    }
    pt = &g_parts[g_nparts];

    to_w(name, wname, MAX_PATH);
    for (wchar_t *w = wname; *w; w++) {
        if (*w == L'/') {
            *w = L'\\';
        }
    }
    wjoin(full, MAX_PATH, g_root, wname);
    if (!fexists(full)) {
        /* 允许平铺布局: 只取文件名 */
        base = name;
        for (const char *s = name; *s; s++) {
            if (*s == '/' || *s == '\\') {
                base = s + 1;
            }
        }
        to_w(base, wname, MAX_PATH);
        wjoin(full, MAX_PATH, g_root, wname);
    }
    data = read_file(full, &len);
    if (!data) {
        snprintf(g_err, sizeof g_err, "读不到文件: %s", name);
        return -1;
    }
    pt->addr = addr;
    wcsncpy(pt->path, full, MAX_PATH - 1);
    pt->path[MAX_PATH - 1] = 0;
    snprintf(pt->label, sizeof pt->label, "%s", base_of(name));
    pt->data = data;
    pt->size = len;
    g_nparts++;
    return 0;
}

static int load_manifest(void)
{
    wchar_t fa[MAX_PATH];
    uint8_t *buf;
    DWORD len = 0;

    g_nparts = 0;
    wjoin(fa, MAX_PATH, g_root, L"flash_args");
    if (fexists(fa)) {
        buf = read_file(fa, &len);
        if (buf) {
            char *p = (char *)buf;
            char *end = p + len;
            while (p < end && g_nparts < MAX_PARTS) {
                char line[600];
                char *e = p;
                size_t ll;
                char *q;
                char *sp;
                uint32_t addr;
                char *name;
                char *r;

                while (e < end && *e != '\n') {
                    e++;
                }
                ll = (size_t)(e - p);
                if (ll >= sizeof line) {
                    ll = sizeof line - 1;
                }
                memcpy(line, p, ll);
                line[ll] = 0;
                p = (e < end) ? e + 1 : end;

                q = line;
                while (*q == ' ' || *q == '\t' || *q == '\r') {
                    q++;
                }
                if (!*q || *q == '-') {
                    continue;
                }
                sp = strchr(q, ' ');
                if (!sp) {
                    continue;
                }
                *sp = 0;
                addr = (uint32_t)strtoul(q, NULL, 0);
                name = sp + 1;
                while (*name == ' ') {
                    name++;
                }
                r = name + strlen(name);
                while (r > name && (r[-1] == '\r' || r[-1] == ' ' || r[-1] == '\t')) {
                    *--r = 0;
                }
                if (!*name) {
                    continue;
                }
                if (add_part(addr, name) != 0) {
                    free(buf);
                    return -1;
                }
            }
            free(buf);
        }
    }

    if (g_nparts == 0) {
        /* 没有 flash_args: 按固定布局兜底。
         * 旧版本这里写的是 app@0x20000, 那是加入 OTA 双槽之前的老布局 ——
         * 0x20000 现在落在 nvs 分区里 (nvs 0x9000-0x2D000), 照老地址写会清掉用户设置。 */
        static const char *fallback[3][2] = {
            {"bootloader/bootloader.bin", "bootloader.bin"},
            {"partition_table/partition-table.bin", "partition-table.bin"},
            {"music_nano.bin", "music_nano.bin"},
        };
        static const uint32_t addrs[3] = {0x1000u, PT_ADDR, APP0_ADDR};
        for (int i = 0; i < 3; i++) {
            if (add_part(addrs[i], fallback[i][0]) != 0) {
                if (add_part(addrs[i], fallback[i][1]) != 0) {
                    return -1;
                }
            }
        }
        /* 字库可选: 找到就写, 没有也能开机 (中文退化成空白, 见 font_part.c) */
        if (add_part(FONT_ADDR, "font16.bin") != 0) {
            if (add_part(FONT_ADDR, "font.bin") != 0) {
                g_err[0] = 0;   /* 两个都没找到不算错误, 免得后面误报 */
            }
        }
    }

    /* 按地址排序 (闪光灯顺序无关, 但便于显示与重试) */
    for (int i = 1; i < g_nparts; i++) {
        Part tmp = g_parts[i];
        int j = i - 1;
        while (j >= 0 && g_parts[j].addr > tmp.addr) {
            g_parts[j + 1] = g_parts[j];
            j--;
        }
        g_parts[j + 1] = tmp;
    }
    return 0;
}

static const Part *part_at(uint32_t addr)
{
    for (int i = 0; i < g_nparts; i++) {
        if (g_parts[i].addr == addr) {
            return &g_parts[i];
        }
    }
    return NULL;
}

/* 兜底闸门: 名单里的地址必须落在真实分区上。
 *
 * 0x0000-0x30000 这一段只允许四个起点 (bootloader/分区表/phy_init/otadata)。
 * 其余一律拒绝, 因为:
 *   - 0x20000 是"加 OTA 双槽之前 app 的老地址", 旧 flash_args 会带它进来;
 *     现在它压在 nvs 里 (nvs 0x9000-0x2D000), 写进去就是清掉用户设置。
 *   - 0x040000 是上一版布局的 ota_0, 现在是 ota_0 分区内部, 写进去会覆盖 app 前半段。
 *   - 这类错误最难查: 写入成功、回读 MD5 也对, 只有重启才会发现。 */
static const uint32_t k_sys_starts[] = {0x1000u, PT_ADDR, 0x2D000u, 0x2E000u};

static int is_sys_start(uint32_t a)
{
    for (unsigned i = 0; i < sizeof k_sys_starts / sizeof k_sys_starts[0]; i++) {
        if (k_sys_starts[i] == a) {
            return 1;
        }
    }
    return 0;
}

static int check_targets(void)
{
    for (int i = 0; i < g_nparts; i++) {
        uint32_t a = g_parts[i].addr;
        uint32_t e = a + (uint32_t)g_parts[i].size;   /* 开区间末端 */

        if (a < SYS_END && !is_sys_start(a)) {
            snprintf(g_err, sizeof g_err,
                     "%s -> 0x%05lX 不是本布局里的分区起点 (低于 0x30000 只允许 "
                     "bootloader/分区表/phy_init/otadata)",
                     g_parts[i].label, (unsigned long)a);
            return -1;
        }
        if (a < SYS_END && is_sys_start(a) && e > SYS_END) {
            snprintf(g_err, sizeof g_err,
                     "%s 从 0x%05lX 起长度 %lu 越过了 0x30000 (压到 ota_0)",
                     g_parts[i].label, (unsigned long)a, (unsigned long)g_parts[i].size);
            return -1;
        }
        if (e > FLASH_END_ADDR) {
            snprintf(g_err, sizeof g_err,
                     "%s 从 0x%05lX 起长度 %lu 越过了 8MB flash 末尾",
                     g_parts[i].label, (unsigned long)a, (unsigned long)g_parts[i].size);
            return -1;
        }
    }
    return 0;
}

static void describe_firmware(void)
{
    char root[MAX_PATH];
    uint64_t total = 0;

    to_u8(g_root, root, sizeof root);
    SAY("固件目录: %s\n", root);
    for (int i = 0; i < g_nparts; i++) {
        SAY("  %-34s %10lu B  ->  0x%05X\n", g_parts[i].label,
            (unsigned long)g_parts[i].size, g_parts[i].addr);
        total += g_parts[i].size;
    }
    SAY("  %-34s %10llu B  (%.2f MB)\n", "合计", (unsigned long long)total, total / 1048576.0);

    {
        const Part *app = part_at(APP0_ADDR);
        if (app && app->size > 0x90 && get_le32(app->data + 0x20) == 0xABCD5432u) {
            const char *ver = (const char *)app->data + 0x30;
            const char *when = (const char *)app->data + 0x80;
            const char *clock = (const char *)app->data + 0x70;
            SAY("  应用版本: %.32s   编译: %.16s %.16s\n", ver, when, clock);
        }
    }
    SAY("  烧录方式: 原始直写 (不启用固件压缩)\n\n");
}

/* ------------------------------------------------------------------ */
/* 串口                                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    HANDLE h;
    char   name[24];
    int    baud;
    int    dtr;
    int    rts;
} Ser;

static uint8_t g_q[16384];
static int g_qh, g_qt;

static void q_reset(void) { g_qh = g_qt = 0; }
static int  q_len(void)   { return (g_qt - g_qh + (int)sizeof g_q) % (int)sizeof g_q; }

static void q_put(const uint8_t *b, int n)
{
    for (int i = 0; i < n; i++) {
        int nx = (g_qt + 1) % (int)sizeof g_q;
        if (nx == g_qh) {
            break;
        }
        g_q[g_qt] = b[i];
        g_qt = nx;
    }
}

static int q_get(uint8_t *b, int n)
{
    int i = 0;
    while (i < n && q_len() > 0) {
        b[i++] = g_q[g_qh];
        g_qh = (g_qh + 1) % (int)sizeof g_q;
    }
    return i;
}

static void ser_pump(Ser *s)
{
    uint8_t buf[2048];
    DWORD got = 0;
    if (!s->h || s->h == INVALID_HANDLE_VALUE) {
        return;
    }
    if (ReadFile(s->h, buf, sizeof buf, &got, NULL) && got) {
        q_put(buf, (int)got);
    }
}

static void ser_dtr(Ser *s, int on)
{
    if (!s->h || s->h == INVALID_HANDLE_VALUE) {
        return;
    }
    s->dtr = on;
    EscapeCommFunction(s->h, on ? SETDTR : CLRDTR);
}

static void ser_rts(Ser *s, int on)
{
    if (!s->h || s->h == INVALID_HANDLE_VALUE) {
        return;
    }
    s->rts = on;
    EscapeCommFunction(s->h, on ? SETRTS : CLRRTS);
    /* usbser.sys 需要一次 DTR 变化才能把 RTS 真正下发 */
    EscapeCommFunction(s->h, s->dtr ? SETDTR : CLRDTR);
}

/*
 * 应用 DCB。DTR/RTS 的控制位必须跟随我们记录的线状态: 否则每次
 * SetCommState(比如切波特率) 都会按 DTR_CONTROL_ENABLE 把 DTR 拉低/拉高,
 * 在这个板子上等于给 EN 一个复位脉冲, 芯片会重启导致波特率切换后失联。
 */
static int ser_apply_dcb(Ser *s, int baud)
{
    DCB dcb;
    memset(&dcb, 0, sizeof dcb);
    dcb.DCBlength = sizeof dcb;
    if (!GetCommState(s->h, &dcb)) {
        return -1;
    }
    dcb.BaudRate = (DWORD)baud;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fAbortOnError = FALSE;
    dcb.fDtrControl = s->dtr ? DTR_CONTROL_ENABLE : DTR_CONTROL_DISABLE;
    dcb.fRtsControl = s->rts ? RTS_CONTROL_ENABLE : RTS_CONTROL_DISABLE;
    if (!SetCommState(s->h, &dcb)) {
        return -1;
    }
    s->baud = baud;
    return 0;
}

static int ser_set_baud(Ser *s, int baud)
{
    return ser_apply_dcb(s, baud);
}

static int ser_open(Ser *s, const char *name, int baud)
{
    char path[48];
    COMMTIMEOUTS to;

    memset(s, 0, sizeof *s);
    s->h = INVALID_HANDLE_VALUE;
    snprintf(path, sizeof path, "\\\\.\\%s", name);
    snprintf(s->name, sizeof s->name, "%s", name);
    s->h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                       0, NULL);
    if (s->h == INVALID_HANDLE_VALUE) {
        return -1;
    }
    SetupComm(s->h, 65536, 65536);
    s->dtr = 0;
    s->rts = 0;
    if (ser_apply_dcb(s, baud) != 0) {
        CloseHandle(s->h);
        s->h = INVALID_HANDLE_VALUE;
        return -1;
    }
    s->baud = baud;
    ser_dtr(s, 0);
    ser_rts(s, 0);

    memset(&to, 0, sizeof to);
    to.ReadIntervalTimeout = MAXDWORD;
    to.ReadTotalTimeoutMultiplier = 0;
    to.ReadTotalTimeoutConstant = 0;
    to.WriteTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant = 15000;
    SetCommTimeouts(s->h, &to);

    PurgeComm(s->h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    q_reset();
    return 0;
}

static void ser_close(Ser *s)
{
    if (s->h && s->h != INVALID_HANDLE_VALUE) {
        CloseHandle(s->h);
    }
    s->h = INVALID_HANDLE_VALUE;
}

static void ser_flush_input(Ser *s)
{
    if (!s->h || s->h == INVALID_HANDLE_VALUE) {
        return;
    }
    PurgeComm(s->h, PURGE_RXCLEAR);
    q_reset();
}

static int ser_write(Ser *s, const void *data, int len)
{
    const uint8_t *p = (const uint8_t *)data;
    while (len > 0) {
        DWORD written = 0;
        if (!WriteFile(s->h, p, (DWORD)len, &written, NULL) || written == 0) {
            return -1;
        }
        p += written;
        len -= (int)written;
    }
    return 0;
}

static int q_wait_byte(Ser *s, int timeout_ms)
{
    DWORD t0 = GetTickCount();
    uint8_t c;
    for (;;) {
        if (q_len() > 0) {
            q_get(&c, 1);
            return c;
        }
        if ((int)(GetTickCount() - t0) >= timeout_ms) {
            return -1;
        }
        ser_pump(s);
        if (q_len() == 0) {
            Sleep(1);
        }
    }
}

/* ------------------------------------------------------------------ */
/* SLIP + 命令                                                         */
/* ------------------------------------------------------------------ */

static uint8_t g_raw[70000];
static uint8_t g_esc[140000];
static uint8_t g_pkt[8192];
static uint8_t g_bld[70000];

static int slip_wrap(const uint8_t *in, int len, uint8_t *out, int cap)
{
    int o = 0;
    if (o >= cap) {
        return -1;
    }
    out[o++] = 0xC0;
    for (int i = 0; i < len; i++) {
        uint8_t c = in[i];
        if (c == 0xC0) {
            if (o + 2 > cap) {
                return -1;
            }
            out[o++] = 0xDB;
            out[o++] = 0xDC;
        } else if (c == 0xDB) {
            if (o + 2 > cap) {
                return -1;
            }
            out[o++] = 0xDB;
            out[o++] = 0xDD;
        } else {
            if (o + 1 > cap) {
                return -1;
            }
            out[o++] = c;
        }
    }
    if (o >= cap) {
        return -1;
    }
    out[o++] = 0xC0;
    return o;
}

static void dump_hex(const char *prefix, const uint8_t *p, int n)
{
    int lim = n > 24 ? 24 : n;
    printf("%s %d:", prefix, n);
    for (int i = 0; i < lim; i++) {
        printf(" %02x", p[i]);
    }
    if (n > lim) {
        printf(" ...");
    }
    printf("\n");
}

/* 发送一条命令, 不等待响应 */
static int cmd_send(Ser *s, int op, const void *data, int dlen, uint32_t chk)
{
    uint8_t *h = g_raw;
    int total, esc;

    if (8 + dlen > (int)sizeof g_raw) {
        return -1;
    }
    h[0] = 0;
    h[1] = (uint8_t)op;
    h[2] = (uint8_t)(dlen & 0xFF);
    h[3] = (uint8_t)((dlen >> 8) & 0xFF);
    put_le32(h + 4, chk);
    if (dlen > 0 && data) {
        memcpy(h + 8, data, (size_t)dlen);
    }
    total = 8 + dlen;
    esc = slip_wrap(h, total, g_esc, (int)sizeof g_esc);
    if (esc < 0) {
        return -1;
    }
    if (g_trace) {
        dump_hex("TX", h, total);
    }
    return ser_write(s, g_esc, esc);
}

/* 读一个 SLIP 包(不含校验), 返回长度, -1 超时 */
static int slip_read(Ser *s, uint8_t *out, int cap, int timeout_ms)
{
    DWORD t0 = GetTickCount();
    int n = 0;
    int in_packet = 0;
    int esc = 0;

    for (;;) {
        int c;
        if ((int)(GetTickCount() - t0) >= timeout_ms) {
            return -1;
        }
        c = q_wait_byte(s, 20);
        if (c < 0) {
            continue;
        }
        if (c == 0xC0) {
            if (in_packet && n > 0) {
                if (g_trace) {
                    dump_hex("RX", out, n);
                }
                return n;
            }
            n = 0;
            esc = 0;
            in_packet = 1;
            continue;
        }
        if (!in_packet) {
            continue;
        }
        if (esc) {
            if (c == 0xDC) {
                c = 0xC0;
            } else if (c == 0xDD) {
                c = 0xDB;
            }
            esc = 0;
        } else if (c == 0xDB) {
            esc = 1;
            continue;
        }
        if (n >= cap) {
            return -1;
        }
        out[n++] = (uint8_t)c;
    }
}

/*
 * 发送并等待响应.
 * resp_len > 0 时把响应体前 resp_len 字节拷贝到 resp.
 * 返回 0 成功, -1 失败(原因在 g_err).
 */
static int esp_cmd(Ser *s, int op, const void *data, int dlen, uint32_t chk,
                   int resp_len, int timeout_ms, uint8_t *resp, uint32_t *out_val)
{
    DWORD t0;

    g_err[0] = 0;
    if (cmd_send(s, op, data, dlen, chk) != 0) {
        snprintf(g_err, sizeof g_err, "串口写入失败");
        return -1;
    }
    if (timeout_ms <= 0) {
        return 0;
    }
    t0 = GetTickCount();
    for (;;) {
        int n;
        int left = timeout_ms - (int)(GetTickCount() - t0);
        int body_len;
        uint32_t val;

        if (left <= 0) {
            snprintf(g_err, sizeof g_err, "等待响应超时 (op=0x%02x)", op);
            return -1;
        }
        n = slip_read(s, g_pkt, (int)sizeof g_pkt, left);
        if (n < 0) {
            snprintf(g_err, sizeof g_err, "等待响应超时 (op=0x%02x)", op);
            return -1;
        }
        if (n < 8 || g_pkt[0] != 1) {
            continue;
        }
        if (op >= 0 && g_pkt[1] != (uint8_t)op) {
            continue;
        }
        body_len = n - 8;
        val = get_le32(g_pkt + 4);
        if (body_len < resp_len + 2) {
            if (body_len >= 1 && g_pkt[8] != 0) {
                snprintf(g_err, sizeof g_err, "命令失败 (op=0x%02x, 状态 0x%02x/0x%02x)",
                         op, g_pkt[8], body_len >= 2 ? g_pkt[9] : 0);
            } else {
                snprintf(g_err, sizeof g_err, "响应长度异常 (op=0x%02x, %d 字节)", op, body_len);
            }
            return -1;
        }
        if (g_pkt[8 + resp_len] != 0) {
            snprintf(g_err, sizeof g_err, "命令失败 (op=0x%02x, 错误 0x%02x)",
                     op, g_pkt[8 + resp_len]);
            return -1;
        }
        if (resp && resp_len > 0) {
            memcpy(resp, g_pkt + 8, (size_t)resp_len);
        }
        if (out_val) {
            *out_val = val;
        }
        return 0;
    }
}

static uint32_t esp_checksum(const uint8_t *d, int n)
{
    uint32_t st = 0xEF;
    while (n-- > 0) {
        st ^= *d++;
    }
    return st;
}

/* ------------------------------------------------------------------ */
/* 设备操作                                                            */
/* ------------------------------------------------------------------ */

static void classic_reset(Ser *s)
{
    ser_dtr(s, 0);   /* IO0 = HIGH */
    ser_rts(s, 1);   /* EN  = LOW  */
    Sleep(100);
    ser_dtr(s, 1);   /* IO0 = LOW  */
    ser_rts(s, 0);   /* EN  = HIGH */
    Sleep(50);
    ser_dtr(s, 0);   /* IO0 = HIGH */
}

static void hard_reset(Ser *s)
{
    ser_rts(s, 1);
    Sleep(100);
    ser_rts(s, 0);
}

static int sync_once(Ser *s, int timeout_ms)
{
    uint8_t payload[36] = {0x07, 0x07, 0x12, 0x20};
    int got = 0;
    DWORD t0;

    memset(payload + 4, 0x55, 32);
    if (cmd_send(s, ESP_SYNC, payload, 36, 0) != 0) {
        return -1;
    }
    t0 = GetTickCount();
    while (got < 8) {
        int left = timeout_ms - (int)(GetTickCount() - t0);
        int n;
        if (left <= 0) {
            break;
        }
        n = slip_read(s, g_pkt, (int)sizeof g_pkt, left);
        if (n < 0) {
            break;
        }
        if (n >= 8 && g_pkt[0] == 1) {
            got++;
        }
    }
    return got > 0 ? 0 : -1;
}

static int dev_connect(Ser *s, int attempts)
{
    for (int a = 0; a < attempts; a++) {
        if (a > 0) {
            Sleep(30);
        }
        ser_flush_input(s);
        classic_reset(s);
        Sleep(40);
        ser_flush_input(s);
        for (int t = 0; t < 4; t++) {
            if (sync_once(s, 160) == 0) {
                return 0;
            }
            Sleep(30);
        }
    }
    return -1;
}

static int dev_read_reg(Ser *s, uint32_t addr, uint32_t *out)
{
    uint8_t d[4];
    put_le32(d, addr);
    return esp_cmd(s, ESP_READ_REG, d, 4, 0, 0, 3000, NULL, out);
}

static int dev_read_mac(Ser *s, uint8_t mac[6])
{
    uint32_t w2 = 0, w1 = 0;
    uint8_t b[8];

    if (dev_read_reg(s, ESP32_EFUSE_BASE + 0x08, &w2) != 0) {
        return -1;
    }
    if (dev_read_reg(s, ESP32_EFUSE_BASE + 0x04, &w1) != 0) {
        return -1;
    }
    b[0] = (uint8_t)(w2 >> 24);
    b[1] = (uint8_t)(w2 >> 16);
    b[2] = (uint8_t)(w2 >> 8);
    b[3] = (uint8_t)(w2);
    b[4] = (uint8_t)(w1 >> 24);
    b[5] = (uint8_t)(w1 >> 16);
    b[6] = (uint8_t)(w1 >> 8);
    b[7] = (uint8_t)(w1);
    memcpy(mac, b + 2, 6);
    return 0;
}

/* ROM 阶段读取 flash (每次最多 64 字节, 很慢但只读 3KB) */
static int rom_read_flash(Ser *s, uint32_t addr, uint8_t *out, uint32_t len)
{
    static const uint8_t attach[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t body[64];

    if (esp_cmd(s, ESP_SPI_ATTACH, attach, 8, 0, 0, 3000, NULL, NULL) != 0) {
        return -1;
    }
    for (uint32_t off = 0; off < len; off += 64) {
        uint32_t chunk = len - off;
        uint8_t d[8];
        if (chunk > 64) {
            chunk = 64;
        }
        put_le32(d, addr + off);
        put_le32(d + 4, chunk);
        if (esp_cmd(s, ESP_READ_FLASH_SLOW, d, 8, 0, 64, 3000, body, NULL) != 0) {
            return -1;
        }
        memcpy(out + off, body, chunk);
    }
    return 0;
}

static int stub_mem_write(Ser *s, uint32_t addr, const uint8_t *data, int len)
{
    int blocks = (len + STUB_RAM_BLOCK - 1) / STUB_RAM_BLOCK;
    uint8_t d[16];

    put_le32(d, (uint32_t)len);
    put_le32(d + 4, (uint32_t)blocks);
    put_le32(d + 8, STUB_RAM_BLOCK);
    put_le32(d + 12, addr);
    if (esp_cmd(s, ESP_MEM_BEGIN, d, 16, 0, 0, 3000, NULL, NULL) != 0) {
        return -1;
    }
    for (int seq = 0; seq < blocks; seq++) {
        int off = seq * STUB_RAM_BLOCK;
        int n = len - off;
        if (n > STUB_RAM_BLOCK) {
            n = STUB_RAM_BLOCK;
        }
        put_le32(d, (uint32_t)n);
        put_le32(d + 4, (uint32_t)seq);
        put_le32(d + 8, 0);
        put_le32(d + 12, 0);
        memcpy(g_bld, d, 16);
        memcpy(g_bld + 16, data + off, (size_t)n);
        if (esp_cmd(s, ESP_MEM_DATA, g_bld, 16 + n, esp_checksum(data + off, n), 0, 3000,
                    NULL, NULL) != 0) {
            return -1;
        }
    }
    return 0;
}

static int stub_upload(Ser *s)
{
    uint8_t d[8];
    DWORD t0;

    if (stub_mem_write(s, STUB_TEXT_ADDR, stub_text, STUB_TEXT_LEN) != 0) {
        return -1;
    }
    if (STUB_DATA_LEN > 0 && stub_mem_write(s, STUB_DATA_ADDR, stub_data, STUB_DATA_LEN) != 0) {
        return -1;
    }

    put_le32(d, 0);           /* entrypoint != 0 */
    put_le32(d + 4, STUB_ENTRY);
    if (cmd_send(s, ESP_MEM_END, d, 8, 0) != 0) {
        return -1;
    }

    t0 = GetTickCount();
    while ((int)(GetTickCount() - t0) < 3000) {
        int n = slip_read(s, g_pkt, (int)sizeof g_pkt, 400);
        if (n == 4 && memcmp(g_pkt, "OHAI", 4) == 0) {
            return 0;
        }
    }
    snprintf(g_err, sizeof g_err, "stub 没有回 OHAI");
    return -1;
}

static int stub_set_baud(Ser *s, int newbaud)
{
    uint8_t d[8];
    uint32_t magic = 0;
    int old = s->baud;

    put_le32(d, (uint32_t)newbaud);
    put_le32(d + 4, (uint32_t)old);
    /* 和 esptool 一致: 先等 stub 回 ack, 再切主机波特率 */
    (void)esp_cmd(s, ESP_CHANGE_BAUDRATE, d, 8, 0, 0, 1000, NULL, NULL);
    Sleep(30);
    ser_flush_input(s);
    if (ser_set_baud(s, newbaud) != 0) {
        return -1;
    }
    Sleep(60);
    ser_flush_input(s);

    for (int i = 0; i < 5; i++) {
        if (dev_read_reg(s, ESP32_MAGIC_ADDR, &magic) == 0 && magic == ESP32_MAGIC) {
            return 0;
        }
        ser_flush_input(s);
        Sleep(30);
    }
    /* 回退 */
    ser_set_baud(s, old);
    Sleep(60);
    ser_flush_input(s);
    for (int i = 0; i < 5; i++) {
        if (dev_read_reg(s, ESP32_MAGIC_ADDR, &magic) == 0 && magic == ESP32_MAGIC) {
            return -1;
        }
        ser_flush_input(s);
        Sleep(30);
    }
    snprintf(g_err, sizeof g_err, "切换波特率后失去响应");
    return -2;
}

static int stub_erase_region(Ser *s, uint32_t addr, uint32_t len)
{
    uint8_t d[8];
    int tmo = 5000 + (int)(len / 1048576.0 * 30000.0);
    put_le32(d, addr);
    put_le32(d + 4, len);
    return esp_cmd(s, ESP_ERASE_REGION, d, 8, 0, 0, tmo, NULL, NULL);
}

static int stub_erase_chip(Ser *s)
{
    return esp_cmd(s, ESP_ERASE_FLASH, NULL, 0, 0, 0, 120000, NULL, NULL);
}

/* ------------------------------------------------------------------ */
/* 烧录                                                                */
/* ------------------------------------------------------------------ */

static DWORD g_bar_last;

static void show_bar(const char *label, uint64_t done, uint64_t total, DWORD t0, int force)
{
    DWORD now = GetTickCount();
    DWORD ms;
    double sec;
    int pct, cells = 24, fill;
    char bar[40];
    double kbs, eta;

    if (!force && now - g_bar_last < 150) {
        return;
    }
    g_bar_last = now;

    ms = now - t0;
    sec = ms / 1000.0;
    pct = total ? (int)(done * 100 / total) : 100;
    fill = pct * cells / 100;
    kbs = sec > 0.05 ? (done / 1024.0 / sec) : 0.0;
    eta = (kbs > 1.0 && done < total) ? (total - done) / 1024.0 / kbs : 0.0;

    for (int i = 0; i < cells; i++) {
        bar[i] = (i < fill) ? '#' : (i == fill ? '>' : '.');
    }
    bar[cells] = 0;
    if (done < total) {
        printf("\r  %-30s [%s] %3d%%  %5.2f/%5.2f MB  %6.0f KB/s  剩 %4.0fs   ", label, bar, pct,
               done / 1048576.0, total / 1048576.0, kbs, eta);
    } else {
        printf("\r  %-30s [%s] %3d%%  %5.2f/%5.2f MB  %6.0f KB/s            ", label, bar, pct,
               done / 1048576.0, total / 1048576.0, kbs);
    }
}

/* 写一段连续区域: FLASH_BEGIN -> DATA* (不含 END).
 * label 只用于进度条; base/len 是这次写的内容; addr 是 flash 上的目标地址. */
static int flash_range(Ser *s, const char *label, uint32_t addr,
                       const uint8_t *data, uint32_t len, DWORD *elapsed_ms)
{
    int blocks = (int)((len + FLASH_BLOCK_STUB - 1) / FLASH_BLOCK_STUB);
    uint8_t params[20];
    uint8_t *blk;
    DWORD t0;

    put_le32(params, len);
    put_le32(params + 4, (uint32_t)blocks);
    put_le32(params + 8, FLASH_BLOCK_STUB);
    put_le32(params + 12, addr);
    params[16] = 0;
    params[17] = 0;
    params[18] = 0;
    params[19] = 0;
    if (esp_cmd(s, ESP_FLASH_BEGIN, params, 20, 0, 0, 8000, NULL, NULL) != 0) {
        return -1;
    }

    blk = (uint8_t *)malloc((size_t)FLASH_BLOCK_STUB + 16);
    if (!blk) {
        snprintf(g_err, sizeof g_err, "内存不足");
        return -1;
    }

    t0 = GetTickCount();
    for (int seq = 0; seq < blocks; seq++) {
        size_t off = (size_t)seq * FLASH_BLOCK_STUB;
        size_t n = len - off;
        uint32_t chk;
        int tries;

        if (n > (size_t)FLASH_BLOCK_STUB) {
            n = FLASH_BLOCK_STUB;
        }
        memcpy(blk + 16, data + off, n);
        memset(blk + 16 + n, 0xFF, (size_t)FLASH_BLOCK_STUB - n);
        put_le32(blk, FLASH_BLOCK_STUB);
        put_le32(blk + 4, (uint32_t)seq);
        put_le32(blk + 8, 0);
        put_le32(blk + 12, 0);
        chk = esp_checksum(blk + 16, FLASH_BLOCK_STUB);

        for (tries = 0; tries < 3; tries++) {
            if (esp_cmd(s, ESP_FLASH_DATA, blk, 16 + FLASH_BLOCK_STUB, chk, 0, 8000, NULL,
                        NULL) == 0) {
                break;
            }
            Sleep(30);
        }
        if (tries == 3) {
            free(blk);
            return -1;
        }
        show_bar(label, off + n, len, t0, 0);
    }
    free(blk);

    *elapsed_ms = GetTickCount() - t0;
    show_bar(label, len, len, t0, 1);
    printf("  %5.1fs\n", *elapsed_ms / 1000.0);
    return 0;
}

static int flash_finish(Ser *s)
{
    uint8_t stay[4] = {1, 0, 0, 0};   /* 1 = 留在 bootloader, 不自动重启 */
    return esp_cmd(s, ESP_FLASH_END, stay, 4, 0, 0, 60000, NULL, NULL);
}

static int flash_md5_range(Ser *s, uint32_t addr, const uint8_t *data, uint32_t len)
{
    uint8_t d[16];
    uint8_t local[16], remote[16];
    int tmo = 5000 + (int)(len / 1048576.0 * 10000.0);

    md5_buffer(data, len, local);
    put_le32(d, addr);
    put_le32(d + 4, len);
    put_le32(d + 8, 0);
    put_le32(d + 12, 0);
    if (esp_cmd(s, ESP_SPI_FLASH_MD5, d, 16, 0, 16, tmo, remote, NULL) != 0) {
        return -1;
    }
    return memcmp(local, remote, 16) == 0 ? 0 : -1;
}

/* 写一个文件: 按 FLASH_CHUNK_MAX 分段, 每段各自
 *   FLASH_BEGIN -> DATA* -> FLASH_END -> MD5 校验
 * 失败只重写这一段, 最多 3 次.
 *
 * 为什么必须分段: 实测单段超过约 2MB 时, stub 在处理最后一块附近会挂掉 ——
 * FLASH_END 返回 "op=0x04, 错误 0xc4", 且该段尾部几十~几百字节没落盘
 * (esptool 回读 3,266,026 字节逐字节比对: 只有末尾 230 字节不同,
 *  设备侧字库 CRC 直接报 0x34acbc00 != 0xcb07d36e).
 * 1MB 一段时每段约 70s, 全程稳定. */
#define FLASH_CHUNK_MAX (1024u * 1024u)

static int flash_file(Ser *s, const Part *p)
{
    uint32_t off = 0;
    int seg = 0;

    while (off < p->size) {
        uint32_t n = p->size - off;
        uint32_t wlen;
        const uint8_t *src;
        uint8_t *wbuf = NULL;
        int attempt;
        if (n > FLASH_CHUNK_MAX) {
            n = FLASH_CHUNK_MAX;
        }
        wlen = n;
        src  = p->data + off;

        /* 最后一段补齐到 16KB 边界再写: 不补时这一段尾部几十~几百字节不会落盘
         * (设备侧字库 CRC 实测只差末尾 230 字节). 补的是 0xFF, 落在镜像声明
         * 长度之外, 设备不会读, 等于把"会丢的尾巴"换成无害填充. */
        if ((off + n) == p->size && (n % FLASH_BLOCK_STUB) != 0) {
            wlen = (n + FLASH_BLOCK_STUB - 1) / FLASH_BLOCK_STUB * FLASH_BLOCK_STUB;
            wbuf = (uint8_t *)malloc(wlen);
            if (!wbuf) {
                snprintf(g_err, sizeof g_err, "内存不足");
                return -1;
            }
            memcpy(wbuf, src, n);
            memset(wbuf + n, 0xFF, wlen - n);
            src = wbuf;
        }

        if (p->size > FLASH_CHUNK_MAX) {
            printf("\n");
            INFO_LINE("%s 第 %d 段 -> 0x%05X  (%lu B)", p->label, seg + 1,
                      p->addr + off, (unsigned long)wlen);
        }

        for (attempt = 1; attempt <= 3; attempt++) {
            DWORD ms = 0;
            if (attempt > 1) {
                WARN_LINE("重写 %s 第 %d 段 (第 %d 次)", p->label, seg + 1, attempt);
            }
            if (flash_range(s, p->label, p->addr + off, src, wlen, &ms) != 0) {
                ERR_LINE("写入失败: %s", g_err);
                continue;
            }
            if (flash_finish(s) != 0) {
                WARN_LINE("flash end: %s", g_err);
                continue;
            }
            printf("  校验 %-30s ... ", p->label);
            if (flash_md5_range(s, p->addr + off, src, wlen) == 0) {
                col(COL_OK);
                printf("MD5 OK\n");
                col(COL_DEF);
                break;
            }
            col(COL_ERR);
            printf("MD5 不一致 (%s)\n", g_err);
            col(COL_DEF);
        }
        if (wbuf) { free(wbuf); wbuf = NULL; }
        if (attempt > 3) {
            ERR_LINE("%s 第 %d 段连续 3 次失败", p->label, seg + 1);
            return -1;
        }
        off += n;
        seg++;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 串口枚举 / 探测                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    char name[16];
    int  ok;
    char why[64];
    uint8_t mac[6];
} PortInfo;

static int cmp_port(const void *a, const void *b)
{
    const PortInfo *pa = (const PortInfo *)a;
    const PortInfo *pb = (const PortInfo *)b;
    int na = atoi(pa->name + 3);
    int nb = atoi(pb->name + 3);
    return na - nb;
}

static int enum_ports(PortInfo *list, int cap)
{
    HKEY k;
    int n = 0;
    DWORD idx = 0;

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &k) !=
        ERROR_SUCCESS) {
        return 0;
    }
    for (;;) {
        char vname[256];
        char data[64];
        DWORD vlen = sizeof vname;
        DWORD dlen = sizeof data;
        DWORD type = 0;
        LONG r = RegEnumValueA(k, idx++, vname, &vlen, NULL, &type, (LPBYTE)data, &dlen);
        if (r == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (r != ERROR_SUCCESS) {
            continue;
        }
        if (dlen > 3 && _strnicmp(data, "COM", 3) == 0 && n < cap) {
            memset(&list[n], 0, sizeof list[n]);
            strncpy(list[n].name, data, sizeof list[n].name - 1);
            list[n].name[sizeof list[n].name - 1] = 0;
            n++;
        }
    }
    RegCloseKey(k);
    qsort(list, (size_t)n, sizeof list[0], cmp_port);
    return n;
}

static int probe_port(const char *name, PortInfo *info)
{
    Ser s;
    uint8_t mac[6];
    uint32_t magic = 0;

    if (name != info->name) {
        strncpy(info->name, name, sizeof info->name - 1);
        info->name[sizeof info->name - 1] = 0;
    }
    info->ok = 0;
    snprintf(info->why, sizeof info->why, "无响应");
    if (ser_open(&s, name, 115200) != 0) {
        snprintf(info->why, sizeof info->why, "打不开(被占用或无此端口)");
        return -1;
    }
    if (dev_connect(&s, 2) == 0) {
        if (dev_read_reg(&s, ESP32_MAGIC_ADDR, &magic) == 0 && magic == ESP32_MAGIC) {
            if (dev_read_mac(&s, mac) == 0) {
                memcpy(info->mac, mac, 6);
                info->ok = 1;
                snprintf(info->why, sizeof info->why, "ESP32");
            } else {
                snprintf(info->why, sizeof info->why, "读 MAC 失败");
            }
        } else {
            snprintf(info->why, sizeof info->why, "不是 ESP32 (magic=0x%08lX)", (unsigned long)magic);
        }
    }
    hard_reset(&s);
    ser_close(&s);
    return info->ok ? 0 : -1;
}

static void fmt_mac(const uint8_t mac[6], char *out, size_t cap)
{
    snprintf(out, cap, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4],
             mac[5]);
}

static int ask_line(char *out, size_t cap)
{
    if (!fgets(out, (int)cap, stdin)) {
        return -1;
    }
    {
        char *r = out + strlen(out);
        while (r > out && (r[-1] == '\n' || r[-1] == '\r' || r[-1] == ' ')) {
            *--r = 0;
        }
    }
    return 0;
}

static int parse_com(const char *s, char *out, size_t cap)
{
    const char *p = s;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (_strnicmp(p, "COM", 3) == 0) {
        p += 3;
    }
    if (*p < '0' || *p > '9') {
        return -1;
    }
    while (*p == '0' && p[1] >= '0' && p[1] <= '9') {
        p++;
    }
    if (strlen(p) + 4 > cap) {
        return -1;
    }
    snprintf(out, cap, "COM%s", p);
    return 0;
}

static int choose_port(char *out, size_t cap)
{
    PortInfo ports[64];
    int n, hits = 0, hit_idx = -1;
    char line[64];

    if (g_port_opt[0]) {
        snprintf(out, cap, "%s", g_port_opt);
        INFO_LINE("使用指定串口 %s", out);
        return 0;
    }

    n = enum_ports(ports, 64);
    if (n == 0) {
        ERR_LINE("没有发现任何串口");
        SAY("     请检查: USB 数据线(能传数据) / 驱动(CH340 或 CP210x) / 板子是否上电\n");
        return -1;
    }
    if (n == 1) {
        snprintf(out, cap, "%s", ports[0].name);
        OK_LINE("检测到 1 个串口: %s", out);
        return 0;
    }

    INFO_LINE("检测到 %d 个串口, 正在逐个探测设备...", n);
    for (int i = 0; i < n; i++) {
        SAY("     探测 %-6s ... ", ports[i].name);
        probe_port(ports[i].name, &ports[i]);
        if (ports[i].ok) {
            char mac[32];
            fmt_mac(ports[i].mac, mac, sizeof mac);
            SAY("ESP32 (%s)\n", mac);
            hits++;
            hit_idx = i;
        } else {
            SAY("%s\n", ports[i].why);
        }
    }

    if (hits == 1) {
        char mac[32];
        fmt_mac(ports[hit_idx].mac, mac, sizeof mac);
        snprintf(out, cap, "%s", ports[hit_idx].name);
        OK_LINE("自动选中 %s  (MAC %s)", out, mac);
        return 0;
    }

    SAY("\n");
    if (hits > 1) {
        WARN_LINE("探测到 %d 台 ESP32, 无法自动选择", hits);
        for (int i = 0; i < n; i++) {
            if (ports[i].ok) {
                char mac[32];
                fmt_mac(ports[i].mac, mac, sizeof mac);
                SAY("     %-6s  MAC %s\n", ports[i].name, mac);
            }
        }
    } else {
        WARN_LINE("没有探测到 ESP32");
        for (int i = 0; i < n; i++) {
            SAY("     %-6s  %s\n", ports[i].name, ports[i].why);
        }
    }

    SAY("\n请输入要烧录的 COM 号 (直接回车退出): ");
    if (ask_line(line, sizeof line) != 0 || !line[0]) {
        return -1;
    }
    if (parse_com(line, out, cap) != 0) {
        ERR_LINE("输入无效: %s", line);
        return -1;
    }
    INFO_LINE("使用 %s", out);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 自检                                                                */
/* ------------------------------------------------------------------ */

static int selftest(void)
{
    struct {
        const char *s;
        const char *h;
    } vec[] = {
        {"", "d41d8cd98f00b204e9800998ecf8427e"},
        {"a", "0cc175b9c0f1b6a831c399e269772661"},
        {"abc", "900150983cd24fb0d6963f7d28e17f72"},
        {"message digest", "f96b697d7cb7938d525a2f31aaf161d0"},
        {"12345678901234567890123456789012345678901234567890123456789012345678901234567890",
         "57edf4a22be3c955ac49da2e2107b67a"},
    };
    int fail = 0;

    SAY("\n== MD5 ==\n");
    for (unsigned i = 0; i < sizeof vec / sizeof vec[0]; i++) {
        uint8_t d[16];
        char h[33];
        md5_buffer(vec[i].s, strlen(vec[i].s), d);
        md5_hex(d, h);
        printf("  %-30s %s %s\n", i < 4 ? (i == 0 ? "(empty)" : vec[i].s) : "(80 digits)", h,
               strcmp(h, vec[i].h) == 0 ? "OK" : "FAIL");
        if (strcmp(h, vec[i].h) != 0) {
            fail++;
        }
    }

    SAY("\n== checksum ==\n");
    {
        const char *t = "123456789";
        printf("  checksum(\"123456789\") = 0x%08lX\n", (unsigned long)esp_checksum((const uint8_t *)t, 9));
    }

    SAY("\n== sync packet ==\n");
    {
        uint8_t payload[36] = {0x07, 0x07, 0x12, 0x20};
        uint8_t raw[64];
        uint8_t esc[128];
        int nn, en;
        memset(payload + 4, 0x55, 32);
        raw[0] = 0;
        raw[1] = ESP_SYNC;
        raw[2] = 36;
        raw[3] = 0;
        put_le32(raw + 4, 0);
        memcpy(raw + 8, payload, 36);
        nn = 44;
        printf("  raw:");
        for (int i = 0; i < 12; i++) {
            printf(" %02x", raw[i]);
        }
        printf(" ...\n");
        en = slip_wrap(raw, nn, esc, (int)sizeof esc);
        printf("  slip len=%d head:", en);
        for (int i = 0; i < 8; i++) {
            printf(" %02x", esc[i]);
        }
        printf("\n  slip(tail):");
        for (int i = en - 4; i < en; i++) {
            printf(" %02x", esc[i]);
        }
        printf("\n");
    }

    SAY("\n== slip escape ==\n");
    {
        uint8_t in[5] = {0xC0, 0xDB, 0x00, 0xDB, 0xC0};
        uint8_t esc[32];
        int en = slip_wrap(in, 5, esc, (int)sizeof esc);
        printf("  in : c0 db 00 db c0\n  out(%d):", en);
        for (int i = 0; i < en; i++) {
            printf(" %02x", esc[i]);
        }
        printf("\n");
    }

    SAY("\n== flash data packet (16KB block, seq=3) ==\n");
    {
        static uint8_t blk[16 + 16384];
        for (int i = 0; i < 16384; i++) {
            blk[16 + i] = (uint8_t)(i & 0xFF);
        }
        put_le32(blk, 16384);
        put_le32(blk + 4, 3);
        put_le32(blk + 8, 0);
        put_le32(blk + 12, 0);
        printf("  hdr:");
        for (int i = 0; i < 16; i++) {
            printf(" %02x", blk[i]);
        }
        printf("\n  chk=0x%08lX\n", (unsigned long)esp_checksum(blk + 16, 16384));
    }

    SAY("\n== stub ==\n");
    printf("  entry=0x%08lX text=%d@0x%08lX data=%d@0x%08lX\n", (unsigned long)STUB_ENTRY,
           STUB_TEXT_LEN, (unsigned long)STUB_TEXT_ADDR, STUB_DATA_LEN,
           (unsigned long)STUB_DATA_ADDR);
    {
        uint8_t d[16];
        char h[33];
        md5_buffer(stub_text, STUB_TEXT_LEN, d);
        md5_hex(d, h);
        printf("  md5(stub_text) = %s\n", h);
        md5_buffer(stub_data, STUB_DATA_LEN, d);
        md5_hex(d, h);
        printf("  md5(stub_data) = %s\n", h);
    }

    SAY("\n%s\n", fail ? "自检失败" : "自检完成");
    return fail ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* 主流程                                                              */
/* ------------------------------------------------------------------ */

static int do_flash(Ser *s, int layout_known, int layout_diff)
{
    int need_erase = 0;
    DWORD t_start = GetTickCount();
    uint64_t total_bytes = 0;
    char line[64];

    /* 是否需要擦除 */
    if (g_force_erase) {
        need_erase = 1;
    } else if (layout_known && layout_diff && !g_no_erase) {
        WARN_LINE("设备上的分区表与本固件不同 (旧固件, 首次升级)");
        SAY("     需要清除 0x8000-0x22000 的旧分区表/设置区/otadata\n");
        SAY("     会清空: 音量/亮度/熄屏时间/喜欢列表/播放记录 (SD 卡不受影响)\n");
        SAY("     继续并清除? [Y/n] ");
        if (ask_line(line, sizeof line) == 0 && (line[0] == 'n' || line[0] == 'N')) {
            WARN_LINE("已取消");
            return 1;
        }
        need_erase = 1;
    } else if (layout_known && !layout_diff) {
        OK_LINE("分区表一致, 保留设备设置");
    } else if (!layout_known) {
        WARN_LINE("无法读取设备分区表, 跳过比对 (若是旧固件请加 /erase)");
    }

    /* 上传 stub */
    SAY("\n");
    INFO_LINE("上传 stub ...");
    if (stub_upload(s) != 0) {
        ERR_LINE("上传 stub 失败: %s", g_err);
        return -1;
    }
    OK_LINE("stub 已运行");

    /* 波特率 */
    if (s->baud < g_baud) {
        int want[3];
        int nwant = 0;
        want[nwant++] = g_baud;
        if (g_baud > 460800) {
            want[nwant++] = 460800;
        }
        if (g_baud > 115200) {
            want[nwant++] = 115200;
        }
        for (int i = 0; i < nwant; i++) {
            int r;
            if (want[i] == s->baud) {
                break;
            }
            r = stub_set_baud(s, want[i]);
            if (r == 0) {
                OK_LINE("波特率 %d", want[i]);
                break;
            }
            if (r == -2) {
                ERR_LINE("%s", g_err);
                return -1;
            }
            WARN_LINE("切换 %d 失败, 尝试下一档", want[i]);
        }
    }

    if (g_erase_region_set) {
        INFO_LINE("擦除区域 0x%05X + 0x%05X ...", g_erase_addr, g_erase_len);
        if (stub_erase_region(s, g_erase_addr, g_erase_len) != 0) {
            ERR_LINE("擦除失败: %s", g_err);
            return -1;
        }
        OK_LINE("擦除完成");
        return 2;
    }

    /* 擦除 */
    if (need_erase) {
        if (g_force_erase) {
            INFO_LINE("整片擦除 (8MB), 请稍候 ...");
            if (stub_erase_chip(s) != 0) {
                ERR_LINE("整片擦除失败: %s", g_err);
                return -1;
            }
            OK_LINE("整片擦除完成");
        } else {
            /* 分两段擦, 中间留出 nvs 的用户数据:
             *   0x8000-0x9000   旧分区表 (新表写下去就覆盖, 但旧表可能更长, 先清干净)
             *   0x1F000-0x30000 旧 otadata(2 扇区) + 旧 phy_init + nvs 扩容新增的尾部。
             * 为什么必须擦到 0x30000:
             *   - otadata 现在在 0x2E000 两个扇区, 旧记录在 0x1F000; 旧的那条 seq 更大,
             *     不清掉 bootloader 会去启动一个根本没写的槽;
             *   - nvs 从 88KB 扩到 144KB 后, 新增的 0x1F000-0x2D000 原来躺着旧
             *     otadata/phy_init 的字节, 不擦干净 NVS 会把它们当本分区的页去解析。
             * 不擦 0x9000-0x1F000: 那是 nvs 现有的 88KB 用户数据。 */
            INFO_LINE("清除旧分区表 (0x8000-0x9000) ...");
            if (stub_erase_region(s, 0x8000u, 0x1000u) != 0) {
                ERR_LINE("擦除失败: %s", g_err);
                return -1;
            }
            INFO_LINE("清除旧 otadata/phy_init + nvs 扩展区 (0x1F000-0x30000) ...");
            if (stub_erase_region(s, 0x1F000u, 0x11000u) != 0) {
                ERR_LINE("擦除失败: %s", g_err);
                return -1;
            }
            OK_LINE("清除完成");
        }
    }

    /* 写文件: 每个文件按 1MB 分段, 每段 FLASH_BEGIN -> DATA* -> FLASH_END -> MD5,
     * 失败只重写该段 (见 flash_file 的注释: 单段超过约 2MB 时 stub 会在末尾挂掉,
     * FLASH_END 报 op=0x04 错误 0xc4, 且该段尾部几百字节没落盘). */
    for (int i = 0; i < g_nparts; i++) {
        printf("\n");
        INFO_LINE("写入 %s -> 0x%05X  (%lu B)", g_parts[i].label, g_parts[i].addr,
                  (unsigned long)g_parts[i].size);
        if (flash_file(s, &g_parts[i]) != 0) {
            return -1;
        }
        total_bytes += g_parts[i].size;
    }


    {
        DWORD ms = GetTickCount() - t_start;
        printf("\n");
        OK_LINE("烧录完成  用时 %.1f s  平均 %.0f KB/s", ms / 1000.0,
                total_bytes / 1024.0 / (ms / 1000.0 + 0.001));
        SAY("     设置区: %s\n", need_erase ? "已清除" : "已保留");
    }
    return 0;
}

int main(int argc, char **argv)
{
    Ser ser;
    char port[32];
    char macstr[32];
    uint8_t mac[6];
    int rc = 1;
    uint8_t *dev_pt = NULL;
    int layout_known = 0, layout_diff = 0;

    memset(&ser, 0, sizeof ser);
    ser.h = INVALID_HANDLE_VALUE;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!_stricmp(a, "/erase") || !_stricmp(a, "-erase") || !_stricmp(a, "--erase")) {
            g_force_erase = 1;
        } else if (!_stricmp(a, "/noerase") || !_stricmp(a, "--noerase")) {
            g_no_erase = 1;
        } else if (!_stricmp(a, "/nopause") || !_stricmp(a, "--nopause")) {
            g_nopause = 1;
        } else if (!_stricmp(a, "/trace") || !_stricmp(a, "--trace")) {
            g_trace = 1;
        } else if (!_stricmp(a, "/selftest") || !_stricmp(a, "--selftest")) {
            g_selftest = 1;
        } else if (!_stricmp(a, "/probe") || !_stricmp(a, "--probe")) {
            g_probe_only = 1;
        } else if ((!_stricmp(a, "/erase-region") || !_stricmp(a, "--erase-region")) &&
                   i + 2 < argc) {
            g_erase_addr = (uint32_t)strtoul(argv[++i], NULL, 0);
            g_erase_len = (uint32_t)strtoul(argv[++i], NULL, 0);
            g_erase_region_set = 1;
        } else if ((!_stricmp(a, "/port") || !_stricmp(a, "--port")) && i + 1 < argc) {
            snprintf(g_port_opt, sizeof g_port_opt, "%s", argv[++i]);
        } else if ((!_stricmp(a, "/baud") || !_stricmp(a, "--baud")) && i + 1 < argc) {
            g_baud = atoi(argv[++i]);
            if (g_baud < 115200) {
                g_baud = 115200;
            }
        } else if (!_stricmp(a, "/help") || !_stricmp(a, "/?") || !_stricmp(a, "-h")) {
            con_init();
            usage();
            return 0;
        }
    }

    con_init();
    col(COL_INFO);
    printf("\n  %s  v%s\n", APP_TITLE, APP_VER);
    col(COL_DEF);
    printf("  ------------------------------------------------------------\n");

    if (g_selftest) {
        rc = selftest();
        pause_exit();
        return rc;
    }

    if (find_root() != 0) {
        ERR_LINE("没有找到固件文件 (flash_args / music_nano.bin)");
        SAY("     请把 music_flash.exe 放在固件目录里再运行\n");
        rc = 1;
        goto out;
    }
    if (load_manifest() != 0) {
        ERR_LINE("读取固件清单失败: %s", g_err);
        rc = 1;
        goto out;
    }
    if (check_targets() != 0) {
        ERR_LINE("拒绝烧录: %s", g_err);
        SAY("     低于 0x30000 只允许 bootloader(0x1000) / 分区表(0x8000) / phy_init(0x2D000)\n");
        SAY("     / otadata(0x2E000); app 从 0x30000 起, 字库从 0x5F0000 起。\n");
        SAY("     这份 flash_args 是旧分区布局留下的, 用 tools\\flash_step.ps1 重新摆镜像。\n");
        rc = 1;
        goto out;
    }
    describe_firmware();

    if (choose_port(port, sizeof port) != 0) {
        rc = 1;
        goto out;
    }

    /* 连接 */
    {
        uint32_t magic = 0;
        for (;;) {
            int connected = 0;
            magic = 0;
            INFO_LINE("打开 %s (115200) ...", port);
            if (ser_open(&ser, port, 115200) != 0) {
                ERR_LINE("打不开 %s", port);
                SAY("     可能被其他软件占用 (串口监视器 / 另一个烧录窗口)\n");
            } else {
                SAY("     复位进入下载模式 ...\n");
                if (dev_connect(&ser, 5) == 0) {
                    if (dev_read_reg(&ser, ESP32_MAGIC_ADDR, &magic) == 0 &&
                        magic == ESP32_MAGIC) {
                        connected = 1;
                    } else if (magic != 0) {
                        ERR_LINE("设备不是 ESP32 (magic=0x%08lX)", (unsigned long)magic);
                    }
                }
                if (connected) {
                    break;
                }
                ser_close(&ser);
                if (magic == 0) {
                    ERR_LINE("无法与设备握手");
                }
            }
            SAY("     1) 数据线要能传数据  2) 装 CH340/CP210x 驱动\n");
            SAY("     3) 按住 BOOT 再点一下 RST/EN, 松开后回车重试 (输入 q 退出)\n");
            SAY("     回车重试 / q 退出: ");
            {
                char line[32];
                if (ask_line(line, sizeof line) != 0 || line[0] == 'q' || line[0] == 'Q') {
                    rc = 1;
                    goto out;
                }
            }
        }
    }

    if (dev_read_mac(&ser, mac) == 0) {
        fmt_mac(mac, macstr, sizeof macstr);
        OK_LINE("已连接 ESP32  (MAC %s)", macstr);
    } else {
        OK_LINE("已连接 ESP32");
    }

    /* 读设备分区表 */
    dev_pt = (uint8_t *)malloc(PT_LEN);
    if (dev_pt) {
        if (rom_read_flash(&ser, PT_ADDR, dev_pt, PT_LEN) == 0) {
            const Part *pt = part_at(PT_ADDR);
            layout_known = 1;
            if (pt && pt->size >= PT_LEN) {
                layout_diff = memcmp(dev_pt, pt->data, PT_LEN) != 0;
            } else if (pt) {
                layout_diff = 1;
            } else {
                layout_known = 0;
            }
        } else {
            WARN_LINE("读取设备分区表失败 (%s)", g_err);
        }
    }

    if (g_probe_only) {
        if (layout_known) {
            OK_LINE("设备分区表与本固件%s", layout_diff ? "不同 (需要先擦除)" : "一致");
        }
        INFO_LINE("探测模式, 不烧录");
        hard_reset(&ser);
        rc = 0;
        goto out;
    }

    {
        int fr = do_flash(&ser, layout_known, layout_diff);
        if (fr == 0 || fr == 2) {
            INFO_LINE("复位设备 ...");
            hard_reset(&ser);
            rc = 0;
            if (fr == 2) {
                SAY("\n");
                OK_LINE("操作完成");
            }
        } else {
            rc = 1;
            SAY("\n");
            ERR_LINE("烧录未完成");
            hard_reset(&ser);
        }
    }

out:
    if (ser.h != INVALID_HANDLE_VALUE) {
        ser_close(&ser);
    }
    if (dev_pt) {
        free(dev_pt);
    }
    for (int i = 0; i < g_nparts; i++) {
        free(g_parts[i].data);
    }
    if (rc == 0) {
        SAY("\n");
        OK_LINE("全部完成, 设备已重启");
    }
    pause_exit();
    return rc;
}
