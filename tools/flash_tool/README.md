# music_flash.exe — music_nano 专用烧录工具

只服务本仓库这套固件（`bootloader@0x1000` / `分区表@0x8000` / `ota_0@0x30000` /
`ota_1@0x280000` / `font@0x4D0000`，见仓库根 `partitions.csv`）。
单文件 exe，不依赖 Python、不依赖 esptool、不依赖驱动以外的任何运行库。

定位：**只做芯片全空时的首次全量烧录，以及测试**。日常升级走 OTA，不经过这里。

发布名是 `一键烧录.exe`（`out\music_flash.exe` 只是编译产物名，改名不影响功能：
程序是相对自身所在目录找固件的）。

## 用法

双击 `music_flash.exe` 就行。也可以：

```
music_flash.exe                 自动检测串口并烧录
music_flash.exe /port COM5      指定串口
music_flash.exe /baud 460800    指定波特率 (默认 921600)
music_flash.exe /erase          强制整片擦除后再烧录
music_flash.exe /noerase        即使分区表不同也直写(不清设置)
music_flash.exe /nopause        结束不等待按键
music_flash.exe /probe          只连接、显示芯片/分区表信息，不烧录
music_flash.exe /trace          打印收发报文(调试)
music_flash.exe /selftest       协议自检(不接触设备)
music_flash.exe /erase-region 0x7F0000 0x10000   调试: 只擦除一段
```

## 行为

1. 在 exe 所在目录（及其上一层、子目录）找 `flash_args` / `music_nano.bin`，
   按 `flash_args` 里的地址烧三个文件；没有 `flash_args` 就用内置默认地址。
2. 串口选择：
   - 0 个 → 提示检查数据线/驱动，按任意键退出
   - 1 个 → 直接用
   - 多个 → 逐个进下载模式读 MAC：命中 1 台自动选中；命中多台列出 MAC 让你输 COM 号；
     一台都没命中就列出全部端口让你输 COM 号
3. 烧录前先读设备 `0x8000` 的 3 KB 分区表与本固件比对：
   - 一致 → 直接覆盖，NVS 设置（音量/亮度/喜欢列表）保留
   - 不同（旧固件升级）→ 提示清一次 `0x8000-0x20000`，默认擦除后再烧
   - `/erase` 则是整片擦除
4. 直连 ROM → 上传 flasher stub → 切 921600 → **原始直写（不压缩）** → 每个文件回读 MD5 校验 → 硬复位。
   失败会先打印原因，再"按任意键结束"，不会一闪而过。

## 速度说明

这颗 flash（Puya，`0x85 0x6017`，8 MB）的擦除很慢，整包 5 MB 约 **5~6 分钟**（≈14 KB/s）。
用 esptool 加 `--no-compress` 是同样的速度（实测 32 KB 2.2 s ≈ 118 kbit/s）；串口本身能跑到
665 kbit/s（读 64 KB 0.8 s），瓶颈在 flash 擦除，不在串口，也不在压缩开关。

## 编译

需要 MSYS2 MinGW-w64 的 gcc（在 PATH 里，或装在默认位置 `C:\msys64\mingw64\bin`；
也可以用 `GCC` 环境变量指定）：

```
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

产物 `out\music_flash.exe`（约 72 KB）。

`src\stub_esp32.h` 是 `tools\gen_stub_h.py` 从 esptool 的
`targets\stub_flasher\1\esp32.json` 生成的（ESP32 flasher stub，GPL-2.0-or-later）。
协议实现参考 esptool 4.12：SLIP 帧、命令校验和（0xEF 异或）、MEM_*、FLASH_*、
`SPI_FLASH_MD5`、`READ_FLASH_SLOW`(64 B/次)。

## 目录

```
flash_tool\
  src\music_flash.c     主程序
  src\md5.c  md5.h      MD5
  src\stub_esp32.h      内嵌 stub (自动生成)
  tools\gen_stub_h.py   stub 头文件生成脚本
  build.ps1             一键编译
  out\music_flash.exe   编译产物
  tmp\                  编译临时文件
```
