# OTA 上机测试记录

**日期**：2026-11-06
**固件**：0.7.0 / 0.7.1 / 0.7.2 / 0.7.99（0.7.99 是专用测试件，`TEST_CRASH_ON_BOOT` 启动即 abort）
**硬件**：ESP32-D0WDR2-V3 / 8MB flash / 2MB PSRAM / 串口 @921600

> ⚠️ **本文记录的是 0.7.x 的旧分区布局**（`ota_1@0x280000` / `font@0x4D0000`，字库 4bpp）。
> 0.7.13 起布局改为 `ota_1@0x310000` / `font@0x5F0000`、字库 2bpp。下文里的地址和 "50 页 / 3200KB" 都只对应当时那版
> 固件，不要照抄。

## 一、结论速览

| 项 | 结果 |
|---|---|
| 字库拆到独立分区，开机 mmap | ✅ 28877 字形，50 页 / 3200KB |
| 全量烧录后 app 瘦身 | ✅ 5,013,280 → 1,765,136 B，槽占 23% |
| 双槽 + otadata 启动选择 | ✅ 按 `(seq-1)%2` 正确切槽 |
| `ota boot <n>` 命令行切槽 | ✅ 校验镜像后写 otadata，1s 后重启 |
| 首启识别 + 15 秒校验窗口 | ✅ 遮罩 + 倒计时 + 禁止睡眠 + 自动打标 |
| 全量烧录不弹窗（出厂态） | ✅ bootloader 直接写 VALID |
| 新固件崩溃自动回退 | ✅ panic → ABORTED → 退回上一槽 |
| **T2 坏镜像被拒（0.7.4）** | ✅ `ESP_ERR_OTA_VALIDATE_FAILED`，不写 otadata、不重启 |
| **T6 字库分区失效（0.7.4）** | ✅ 退 ASCII，不崩，`font crc` 报 state=2 |
| **T7 系统层遮罩（0.7.4）** | ✅ `lv_layer_sys()`，15 秒无残影 |

## 二、实测过程

### T0 全量烧录 → 启动

```
boot:  5 font  Unknown data  01 40 004a0000 00340000
boot: Loaded app from partition at offset 0x40000
app_init: App version: 0.7.0
font: 已映射: 28877 字形, dsc 462032 B, bitmap 2803930 B, 共 50 页 (3200 KB)
[OTA] 运行 ota_0 @0x40000 v0.7.0 state=VALID 本次复位=上电(1) 字库=0(ok)
[OTA] 不是首次启动的新固件 (state=VALID), 无需校验     ← 出厂态不弹窗
```

### T1 命令升级（正常）

```
[OTA] 已写 otadata: 下次从 ota_1 @0x270000 (v0.7.1) 启动, 1 秒后重启
boot: Loaded app from partition at offset 0x270000
app_init: App version: 0.7.1
[OTA] 运行 ota_1 @0x270000 v0.7.1 state=PENDING_VERIFY 本次复位=软件重启(3)
POWER: 睡眠抑制 开启
OTA: 检测到新固件首次启动, 进入 15 秒校验窗口
OTA: 开始 15 秒校验窗口 (禁止睡眠)                    ← 遮罩出现
I (16335) OTA: 校验通过, 已标记 VALID                 ← 15s 后
I (16335) POWER: 睡眠抑制 解除
```

### T4 新固件崩溃 → 自动回退

```
[OTA] 已写 otadata: 下次从 ota_1 @0x270000 (v0.7.99) 启动
app_init: App version: 0.7.99
[TEST] 故意在启动最早期崩溃 (TEST_CRASH_ON_BOOT)
abort() was called at PC 0x400d653d on core 0
Rebooting...
-------- 复位后 --------
boot: Loaded app from partition at offset 0x40000    ← 自动退回 ota_0
[OTA] 运行槽: ota_0 启动槽: ota_0 下次升级写: ota_1
[OTA] 槽0 ota_0 v0.7.0  state=VALID   <- 正在跑
[OTA] 槽1 ota_1 v0.7.99 state=ABORTED
```

回退由 bootloader 完成（`otadata` 里 PENDING_VERIFY 在下次启动被判 ABORTED，
选择时跳过），应用侧零参与。

### T2 坏镜像被拒（0.7.4）

往**非运行槽** ota_0 写 64KB 全零（相当于"有内容但不是镜像"），然后试图切过去：

```
[OTA] 运行槽: ota_1 @0x270000   启动槽: ota_1   下次升级写: ota_0
[OTA] 槽0 ota_0 @0x40000  v-           state=VALID
[OTA] 槽1 ota_1 @0x270000  v0.7.4       state=VALID   <- 正在跑
[OTA] 切到 ota_0 失败: ESP_ERR_OTA_VALIDATE_FAILED (镜像无效或未烧写)
```

三条断言都成立：

1. `esp_ota_set_boot_partition()` 自己会验镜像（magic/段校验和/SHA256），
   拒绝后**没有写 otadata、没有重启**，设备继续跑 ota_1。
2. **`state=VALID` 不代表镜像完好** —— 那只是 otadata 里上一次打标的记录，
   它不校验内容。所以"切槽前重新验一遍"这个动作是必需的，不能省。
3. 关机重启也安全：otadata 仍指向 ota_1，坏掉的 ota_0 不会被选中。

### T6 字库分区失效（0.7.4）

擦掉 font 分区头部 64KB（`erase -Addr 0x4A0000 -Len 0x10000`）后复位：

```
E (682) font: 字库不可用 (magic 不对 (0xffffffff), 分区没烧字库?), 已退化为 ASCII 字体
[OTA] 运行 ota_1 @0x270000  v0.7.4  state=VALID  本次复位=上电(1)  字库=2(magic 不对 …)
I (918) LVGL: LVGL is OK
I (939) SDMMC: SD 卡已挂载
[字库] 不可用 (state=2): magic 不对 (0xffffffff), 分区没烧字库?     ← font crc 命令
```

结论：**字库坏了不影响程序运行、不崩**。中文变成空白，英文数字正常，
菜单/播放/SD 全都照常。`font crc` 手动命令能明确指出坏在哪。

### T7 系统层遮罩（0.7.4）

`ota boot 1` 切到新槽，首启进 15 秒窗口：

```
[OTA] 运行 ota_1 @0x270000  v0.7.4  state=PENDING_VERIFY  本次复位=软件重启(3)
I (720) POWER: 睡眠抑制 开启
W (1030) OTA: 开始 15 秒校验窗口 (禁止睡眠)
I (16057) OTA: 校验通过, 已标记 VALID
I (16057) POWER: 睡眠抑制 解除
```

遮罩挂 **`lv_layer_sys()`（系统层）** 而不是 `lv_layer_top()`，原因是本工程的
界面切换动画（`mode_anim` / `menu_anim` / `ui_player`）都在 `lv_layer_top` 上放
全屏快照图，`mode_anim` 还会把 `lv_scr_act()` 整屏 `HIDDEN`。按 LVGL 8 的顺序
（绘制 `scr → top → sys`，触摸搜索 `sys → top → scr`），只有 sys 层能保证
遮罩永远在最上、且第一个吃掉触摸、并且跨 `lv_scr_load` 存活。

## 三、过程中发现并修掉的问题

### 1. 字库写坏（严重，已修）

**现象**：字库分区写完，烧录器报 `MD5 不一致`，设备侧 `font_part` 报
`CRC 校验失败 (bin=0xcb07d36e, flash=0x34acbc00)`，中文退化成 ASCII。

**定位**：用 esptool 独立回读 3,266,026 字节与原文件逐字节比对 —— 只有
**末尾 230 字节**（0x31D500–0x31D5E9）不同，其余完全一致。

**根因**（一次 `FLASH_BEGIN → DATA* → FLASH_END` 段里写的内容太多）：
单段超过约 2MB 时，stub 在处理最后一块附近就挂了 —— `FLASH_END` 返回
`op=0x04, 错误 0xc4`，而且该段最后一个不满 16KB 的块，尾部几十~几百字节
没有落盘。症状与"文件数""波特率""会话时长"都无关，只与**单段大小**有关：
1.68MB 的 app 一次写完没问题，3.11MB 的字库必挂。

**修复（已进工具，不是脚本绕过）**：`tools/flash_tool/src/music_flash.c`
的 `flash_file()` 现在按 **1MB 分段**，每段各自
`FLASH_BEGIN → DATA* → FLASH_END → MD5 校验`，失败只重写该段（最多 3 次）；
最后一段写之前**补齐到 16KB 边界**（补 0xFF，落在镜像声明长度之外，设备不读）。
实测 3,266,026 字节的字库单文件烧写：4 段全部 `MD5 OK`，退出码 0，
设备侧 `全量 CRC 通过 (0xcb07d36e, 271 ms)`。

### 2. `flash_step.ps1` 自身的两个 bug（已修）

- splat 用了 PowerShell 自动变量 `$args`（空数组）→ 参数根本没传给烧录器，
  没有 `/nopause` 才卡在"按任意键结束"
- 地址格式化拼出 `0x0x4A0000`，导致 font_a 被写到 0x0000，覆盖了 bootloader
  和分区表（一次完整擦写后恢复）

### 3. 烧录工具搬进仓库并顺手修掉构建问题

`flash_tool` 从仓库外的一个独立目录移到 `tools/flash_tool`：

- `build.ps1` 用 `$PSScriptRoot` 定位（`$MyInvocation.MyCommand.Path` 在脚本
  里取不到），并对 gcc 调用期间放宽 `$ErrorActionPreference`（PS 5.1 会把
  gcc 的警告当终止错误），源码/输出一律用绝对路径（`Set-Location` 不会同步
  进程工作目录，gcc 是原生进程）
- 产物 `tools/flash_tool/out/music_flash.exe` 仍在 `.gitignore` 里，用
  `tools/flash_tool/build.ps1` 编译

### 4. 同一槽重复 `ota boot` 不触发校验窗口（已修）

`ota boot 1` 时已经在跑 ota_1，以前会照样写 otadata（指向正在运行的分区）。
现在 `ota_switch()` 先判断"目标 == 运行槽"，直接拒绝：

```
[OTA] 已经在跑 ota_1, 不需要切槽
```

### 5. 字库分区的全量 CRC 从启动路径移走（0.7.4，取舍）

原先"刚 OTA 完首启"时对 3.27MB 字库跑一次全量 CRC（271ms）。这件事的时机是
反的：**网络 OTA 永远不写字库分区**，字库只会被烧录器在整片全烧时写一次，
而那一刻的状态是 `VALID`，反而不校验。

本机是可插拔电池，一天十几次冷启动，启动速度优先，所以：

- 启动路径上只保留**纯内存校验**：magic / 版本 / 段长与偏移自洽 / mmap 成功 /
  首末两条 `glyph_dsc` 抽检。全是微秒级，冷启动一毫秒都不多花。
- 全量 CRC 改成手动命令 `font crc`（`font_part_verify_crc()`），271ms 只在
  需要时付。

**已接受的残余风险**：flash 数据区位翻转会让 `glyph_dsc.bitmap_index` 变成野值，
理论上可能读到 DROM 映射窗口之外触发异常。概率极低，真要穷尽得跑一遍
28,877 条结构扫描（约 20ms），为这个场景不划算。

## 四、未覆盖（受时间/条件限制）

| 项 | 说明 |
|---|---|
| ~~T3 多次升级（4 轮）~~ | ✅ 已跑满，见第七节（4 轮轮转 + 1 次坏固件回退） |
| T5 校验窗口内拔电池 | 已用"窗口内复位"做等价验证（见第六节末）；真拔电池未做 |
| T5 窗口内按电源键 | 设计上被睡眠抑制挡掉，未实测 |

## 五、遗留

- 字库分区单槽无备份；字库只由烧录器全烧时写入，不做在线更新。
- ~~**网络 OTA（app 自己下载 .bin 写进备用槽）尚未实现**~~ → 0.7.8 已实现，见第八节。
- 烧录速度受限于这颗 Puya flash 的擦除（约 14 KB/s），全套 5MB 仍需 6~8 分钟。

## 六、分区改造后复验（0.7.1 / 2026-11-7）

> 上面 T0~T7 的日志都是**旧布局**（`ota_0@0x040000` / `ota_1@0x270000` /
> `font@0x4A0000`）下的记录，保留原文作历史。0.7.1 起地址改成
> `ota_0@0x030000` / `ota_1@0x280000` / `font@0x4D0000`，nvs 88KB→144KB。
>
> 下面日志里的 `v0.8.0` / `v0.8.2` 是**重编号前**编出来的版本串。按现在的编号
> （0.7.0 = OTA 关键功能，0.7.1 = 加入 WiFi），它们分别对应这两条提交。日志正文
> 保留原样，不去改写跑出来的东西。

整片擦除 + 全套烧录（390.1 s，5,575,146 B，5 段全部 `MD5 OK`）后，启动日志：

```
I (53) boot:  0 nvs              WiFi data        01 02 00009000 00024000
I (54) boot:  1 phy_init         RF data          01 01 0002d000 00001000
I (55) boot:  2 otadata          OTA data         01 00 0002e000 00002000
I (55) boot:  3 ota_0            OTA app          00 10 00030000 00250000
I (56) boot:  4 ota_1            OTA app          00 11 00280000 00250000
I (57) boot:  5 font             Unknown data     01 40 004d0000 00330000
I (680) boot: Loaded app from partition at offset 0x30000
I (676) app_init: App version:      0.8.0
I (830) font: 已映射: 28877 字形, dsc 462032 B, bitmap 2803930 B, 共 50 页 (3200 KB)
[OTA] 运行 ota_0 @0x30000  v0.8.0  state=VALID  本次复位=上电(1)  字库=0(ok)
```

字库 mmap 仍是 **50 页**，`.flash.rodata` 涨到 12 页，页池 64 页里还剩 2 页。

**双槽仍然可用**：往 `ota_1` 单刷 app（158.1 s）后 `ota boot 1`：

```
I (680) boot: Loaded app from partition at offset 0x280000
[OTA] 运行 ota_1 @0x280000  v0.8.0  state=PENDING_VERIFY  本次复位=软件重启(3)  字库=0(ok)
W (863) OTA: 检测到新固件首次启动, 进入 15 秒校验窗口
```

**顺带把 T5 的等价场景验了**：上面这次是"新固件首启、窗口没走完就被复位"
（`reset_log.ps1` 直接打了 EN 脉冲）。下一次启动 bootloader 判定未确认 →
`ABORTED` → **自动退回 ota_0**：

```
I (680) boot: Loaded app from partition at offset 0x30000    ← 退回 ota_0
[OTA] 运行 ota_0 @0x30000  v0.8.0  state=VALID  本次复位=上电(1)  字库=0(ok)
```

也就是说"窗口内断电/复位"的作用和"窗口内崩溃"一样，都会退回旧槽 —— 这一条
可以认为 T5 已验证（真·拔电池仍是同一码路径）。

## 七、T3 四轮轮转 + 坏固件回退（0.7.1 / 2026-11-7）

没有网络 OTA 时，"写进另一个槽"用烧录器测试通道模拟（`flash_step.ps1 app2 -Slot N`
+ 串口 `ota boot N`），走的 otadata 路径与 `esp_ota_set_boot_partition()` 完全一样。

| 轮 | 写进 | 镜像 | 结果 |
|---|---|---|---|
| R1 | ota_1 @0x280000 | 0.8.0（2,269,248 B） | PENDING_VERIFY → **VALID**，`下次升级写: ota_0` |
| R2 | ota_0 @0x030000 | 0.8.2 | PENDING_VERIFY → **VALID**，`下次升级写: ota_1` |
| R3 | ota_1 @0x280000 | **启动即 abort 的坏固件**（780,976 B） | 崩溃 → 重启 → **自动退回 ota_0**，槽1 记 `ABORTED` |
| R4 | ota_1 @0x280000 | 0.8.0 | PENDING_VERIFY → **VALID**（`ABORTED` 的槽可以重新用） |

R3 的关键几行：

```
[OTA] 已写 otadata: 下次从 ota_1 @0x280000 (v0.8.2) 启动, 1 秒后重启
[TEST] 故意在启动最早期崩溃 (TEST_CRASH_ON_BOOT)
abort() was called at PC 0x400d7f04 on core 0
Rebooting...
[OTA] 运行 ota_0 @0x30000  v0.8.2  state=VALID  本次复位=软件重启(3)  字库=0(ok)
[OTA] 槽1 ota_1 @0x280000  v0.8.2  state=ABORTED
```

R4 结束后两个槽都是 `VALID`，设备跑在 ota_1 的 0.8.0 上。

**结论**：

- `esp_ota_get_next_update_partition()` 每次都给出**非运行槽**（R1 运行 ota_0→写 ota_1；
  R2 运行 ota_1→写 ota_0；R3 回退后运行 ota_0→R4 又写 ota_1）
- 轮转不是"严格 0/1 交替"，而是"永远写非运行槽"。R3 的回退让运行槽停在 ota_0，
  所以 R4 的目标又回到 ota_1 —— 这是对的，不是漏跳
- 坏固件只污染它自己那个槽，运行槽和 otadata 都没事，回退全自动




## 八、网络 OTA（0.7.8 / 2026-11-12）

前七节的"写进另一个槽"都是烧录器代劳的。这一节是**设备自己上网下载 .bin**。

### 8.1 链路

```
设置 → 固件版本 → [检查更新]
  → 连接 WiFi（扫描/密码/连接，见 ui_settings_wifi.c）
  → 正在检查更新         GET  releases 接口（JSON）
  → 版本页               当前 0.7.8 / 最新 0.7.9 + 「更新」
  → 下载 + 写入           256KB 一块进 PSRAM → 停读 → 4KB 一步写 ota_x
  → 更新完成，正在重启    释放 WiFi 后 esp_restart()
  → 新固件首启            第六/七节那套 15 秒校验窗口 → VALID（没确认就回退）
```

服务器走 GitHub Releases，但**都经 gh-proxy 转发**（`https://gh-proxy.com/` + 原始地址），
国内可直连。两个接口路径固定：

| 用途 | 地址 |
|---|---|
| 版本 | `https://gh-proxy.com/https://api.github.com/repos/coconut-E/music_nano/releases?per_page=5` |
| 下载 | `https://gh-proxy.com/` + JSON 里的 `browser_download_url` |

资产命名约定 `music_nano 0.7.9.bin`（空格/下划线/短横都认），版本号取自**文件名**。
两个串存在 NVS（`ota/api`、`ota/dlp`），串口可以改：

```
ota server                          # 看当前用的是哪个
ota server <api>                    # 只改版本接口（下载前缀清空 = 直接用 JSON 里的绝对地址）
ota server <api> <dlp>              # 同时改下载前缀（GitHub 要 https://gh-proxy.com/）
ota reset                           # 抹掉，回到编译期默认值
```

### 8.2 设备端三个关键决定（都有源码依据）

1. **`esp_ota_begin` 必须传 `OTA_WITH_SEQUENTIAL_WRITES`**。
   给了具体 `image_size` 会先 `esp_partition_erase_range()` 把整包擦掉
   （`esp_ota_ops.c:190`），2.2MB 连续擦除十几秒不可中断。sequential 是
   "写到哪里擦到哪里"（同文件 `:314` 的 `need_erase` 分支）。

2. **PSRAM 指针不能直接喂 `esp_ota_write`**。
   ESP32 主 flash（SPI1）的 `supports_direct_write()` 恒为 false
   （`hal/spi_flash_hal.c:155`），`esp_flash_api.c:1128` 于是走 `direct_write=false`
   分支，退化成 **32 字节一次**的栈上拷贝 —— 2.2MB 就是 6.9 万次 cache 开关。
   所以先 memcpy 进 4KB 内部 RAM，再写。

3. **每 4KB 让出一次 CPU**。写 flash 的窗口里 cache 是关的，另一个核取不到指令；
   一次扇区擦写 25~50ms，连着写就是整机卡死。`vTaskDelay(2ms)` 把停顿切碎，
   界面/音频/蓝牙都能拿到 CPU（`CONFIG_FREERTOS_HZ=1000`，2ms 是 2 个 tick）。

失败路径：**otadata 只在最后一步 `esp_ota_set_boot_partition()` 写一次**。
下载中断/写入失败/校验失败都只会在备份槽里留一段垃圾，下次启动照旧跑老槽。

### 8.3 已经验证过的（主机侧，2026-11-12）

| 项 | 结果 |
|---|---|
| 编译 | `0x244730` = **2,377,520 B**，槽 2,425,600 B，剩 `0xb8d0`（**2%**）|
| TLS 代价 | 2,377,520 − 2,200,256 = **+177,264 B**（esp-tls + mbedtls TLS1.2 + x509）|
| 局域网 mock 服务器 + pin 的 CA | python `ssl.create_default_context(cafile=main/certs/ota_ca.pem)` 走完整 TLS：JSON 200 / 501 B，固件 2,377,520 B |
| gh-proxy 的 API | 200，`[]`（仓库还没有 release）|
| gh-proxy 的资产 | 200，`application/octet-stream`，带 `Content-Length`，**没有 302** |
| gh-proxy 的证书链 | `gh-proxy.com` ← YE2 ← Root YE ← ISRG Root X2 ← **ISRG Root X1**（2035-06-04）|

因为只有 `gh-proxy.com` 一个 TLS 出口，固件里只 pin **ISRG Root X1 + X2**
（外加局域网调试用的自签 CA，三张拼在 `main/certs/ota_ca.pem`，~4KB）。
证书**有效期不校验**（`CONFIG_MBEDTLS_HAVE_TIME_DATE=n`）：本机没有 RTC/SNTP，
开了会让 1970 年的系统时间把所有证书判成"还没生效"。

### 8.4 待上机验证

| # | 场景 | 期望 |
|---|---|---|
| 1 | 局域网 mock 发布与本机同版本 | 「已是最新版本」2 秒 → 回设置 |
| 2 | 发布更高版本 | 版本页显示两个版本 → 点更新 → 进度走 → 自动重启 → 15 秒窗口 → `ota report` 显示切槽 |
| 3 | `build.ps1 -TestCrash` 发布坏固件 | OTA 成功后启动即崩 → bootloader 自动退回旧槽（复用第七节链路）|
| 4 | 下载中途杀掉 mock 服务器 | 「更新失败：下载中断」→ 自动退出，`ota report` 两槽/otadata 均未变 |
| 5 | 换一张 CA 重编 | 「更新失败：证书校验失败」|
| 6 | 升级前后 `free` / `psram` | 暂存块（256KB PSRAM）与弹跳块（4KB 内部）都释放；WiFi 释放后内部堆回到 ~37KB |
| 7 | 写入阶段盯界面 | 进度条 5Hz 在动，最长卡顿应 ≈ 一次 4KB 擦写（目标 < 100ms）|

### 8.5 已知取舍

- 发布后最多 **60 秒**才可见：gh-proxy 走 Cloudflare，API 响应带
  `cache-control: max-age=60`，这不是设备的问题。
- GitHub API 有速率限制，而且 gh-proxy 的出口 IP 是共享的（实测撞到过
  `API rate limit exceeded`）。所以 403 要按"服务器繁忙，稍后再试"处理，
  **不要重试轰炸**。
- 字库分区依然不参与 OTA（`main` 只写 app 槽），改字库仍要烧录器。
- 没做固件签名（secure boot）：信任链 = TLS pin + `esp_ota_end` 的镜像校验和。