# 注释勘误表 / 待确认清单

本次工作原则：**先证明、后注释**。能确证的写入中文注释；发现既有注释错误则按代码修正；
无法确证的**不写注释**，登记在《待确认清单》，由维护者裁决。
查证来源：代码调用链 + ESP-IDF（`$IDF_PATH`）源码 + ID3/FLAC/WAV 规范。

---

## 一、注释勘误表（原名/原内容 → 现内容）

### 第一轮
| # | 文件:行 | 原注释 | 现注释/处理 | 依据 |
|---|---------|--------|-------------|------|
| 1 | `main/storage/sys_monitor.c`（deinit） | "先置空缓存指针再释放, LVGL 并发访问只会读到 NULL" | "内存移交 retire，由 UI 任务 `fs_cache_reap()` 释放，避免读者已取得旧指针（UAF）" | 置 NULL 不能阻止读者持旧指针；修复见 `0f554f2` |
| 2 | `main/audio/audio_task.cpp`（open_decoder） | 封面"提交解码；转移所有权"（无条件 take_cover） | "仅当 `cover_submit_job` 返回 true 才 take_cover，否则解码器 close 释放" | 原逻辑提交失败泄漏；修复见 `29fa53a` |
| 3 | `main/audio/decoder_flac.cpp:62` | `resync_budget` "重同步最大扫描字节数" | "重同步剩余尝试次数" | 代码按错误迭代递减 |
| 4 | `main/audio/decoder_flac.cpp:590` | "最多扫描 64KB 找同步字" | "最多尝试 65536 次 (迭代次数, 非字节数)" | 同上 |
| 5 | `main/ui/menu/menu_browser.c:20` | `FS_ROW_H 18 "实际运行时覆盖为 30"` | **删除该宏**；行高实为硬编码 30 | 全文件无引用；实际值 30 (`fs_row_create`) |
| 6 | `main/ui/menu/menu_browser.c:33` | `s_fs_inside "是否已进入子目录"` | 追加"(仅赋值, 未被读取: 遗留)" | grep 仅见赋值无读取 |
| 7 | `main/ui/ui_player.c:67` | `BRI_BTN_TUNE_CLR "仅供透明前临时占位, 可删"` | "触发按钮底色 (随即设为全透明, 仅调试观察区域用)" | 该宏在 `:1181` 被引用 |
| 8 | `main/app/song_hash.h` 文件头 | "键格式… artist + '\x1f' + title" | 补充"当前只用 `song_hash_name_key()`，未使用 `song_hash_key()`" | 无调用点 |
| 9 | `main/ui/menu/menu_browser.c:355` | "组名 = 父%子" | "组名 = 父/子 (真实 '/')" | `fs_child_group` 实为 `"%s/%s"` |

### 第二轮（本轮）
| # | 文件:行 | 原注释 | 现注释/处理 | 依据 |
|---|---------|--------|-------------|------|
| 10 | `main/app/played_bits.c:26` | `PB_VAL_MAX 4096` 旁注"NVS 字符串上限 4000" | **代码改为 4000**；注释"NVS 字符串含 NUL 上限 4000 字节" | IDF `nvs.h:294` 明确 4000；原值 4096 会使 (4000,4096] 的记录过校验却 `nvs_set_str` 失败 |
| 11 | `main/audio/decoder_mp3.cpp:70` | `ft=(h[4]&0x10)` 注"版本/扩展标志" | **代码改为 `h[5]&0x10`**；注释说明 ID3 flags 在 h[5], `0x10`=footer(v2.4) | ID3v2 头 [3]=版本,[4]=修订,[5]=标志；扩展头已计入 tag size, 仅 footer 需 +10 |
| 12 | `main/audio/decoder_mp3.cpp:112` | "(+ 扩展头)" | "(+ 10 字节 footer, 仅 v2.4; 扩展头已计入 ts)" | 同 #11 |
| 13 | `main/audio/resampler_fxp.cpp:20` | "归一化到 Q15 … int64 累加" | "归一化到 Q14, int32 累加" | 代码 `*16384.0`, `int32_t acc` |
| 14 | `main/audio/resampler_fxp.cpp:33` | `coeff "Q15 系数"` | "Q14 系数" | 同 #13 |
| 15 | `main/audio/resampler_fxp.cpp:116` | "Σ\|c\|≤33517 × 32767" | "Σ\|c\| 约 ≤3.36e4 × 32767" | 精确最大值非 33517；安全结论成立，改为不声称精确值 |
| 16 | `main/audio/cover.cpp:44` | `s_tcb "静态任务 TCB (PSRAM)"` | "静态任务 TCB (必须放内部 RAM)" | 实为 `MALLOC_CAP_INTERNAL` (`:312`) |
| 17 | `main/audio/cover.cpp:151` | "成功返回 1 (已缩放写入 s_out_buf)" | "解码到中间缓冲 s_raw (后续 crop 写入 s_out_buf)" | 解码写 `s_raw`，`cover_scale_crop_to_100` 才写 `s_out_buf` |
| 18 | `main/audio/cover.cpp:160` | "+16 边缘缓冲, 避免 MCU 块越界" | "留 16 行列余量; 真正防越界靠回调内按 s_raw_w/h 钳制" | 回调每笔写入均钳制 |
| 19 | `main/audio/cover.cpp:305` | "常驻缓冲全部放 PSRAM" | "尽量放 PSRAM (TCB 例外, 必须内部 RAM)" | 同 #16 |
| 20 | `main/audio/decoder_flac.cpp:92` | "提取 TITLE=/ARTIST=/ALBUM= 到输出缓冲" | "提取 TITLE/ARTIST 到输出 (ALBUM 仅日志)" | ALBUM 未写入输出 |
| 21 | `main/audio/decoder_flac.cpp:387` | `max_samples "本次最多输出帧数"` | "本次最多输出样本数 (帧×声道)" | 用于 `pcm[i]` 索引与 `cap_samples`（样本）比较 |
| 22 | `main/audio/decoder_flac.cpp:466` | "扫描帧同步字 0xFF F8/F9/FA/FB" | "0xFF 后跟 0xF8/0xF9 (掩码 &0xFE==0xF8)" | 代码 `(b[j+1]&0xFE)==0xF8` 只匹配 F8/F9；FLAC 规定 reserved 位=0 |
| 23 | `main/audio/decoder_flac.cpp:327` | 多声道"继续解码会写越界" | "转换管线只接受单/双声道；提前拒绝打开" | 已加 `cap_samples` 限幅，越界说法不再成立 |
| 24 | `main/audio/decoder_wav.cpp:32` | `data_pos "已交付给调用方的 PCM 字节数"` | "已从 data 块消费的源 PCM 字节数 (16bit 时等于输出)" | `data_pos += n`（源字节），输出另算 |
| 25 | `main/audio/decoder_wav.cpp:54` | "去掉首尾空白" | "去掉尾部空白" | 仅尾部 trim |
| 26 | `main/audio/decoder_wav.cpp:286` | "精确字节率" | "精确码率(kbps)" | 字段名 `bitrate_kbps`，音频任务按 kbps 处理 |
| 27 | `main/audio/audio_task.h:23` | `AUDIO_CMD_SEEK "param=字节偏移"` | "param=千分比 0~1000" | 代码 `target=fsz*param/1000` |
| 28 | `main/audio/audio_task.cpp:66/166` | "全程持锁/防撕裂" | **代码补锁**：`update_duration_elapsed_locked` + SEEK 处加锁，使所有 `g_song_info` 字段写入都在 `s_info_mux` 下 | 原 `:339/:420/:481` 未加锁，与快照承诺矛盾 |
| 29 | `main/audio/audio_task.cpp:424` | "样本数 = 字节/声道数/2" | "帧数 = 字节/(声道数×2字节)" | 变量名 `frames`，API 参数 `src_frames` |
| 30 | `main/audio/audio.h:25` | format "(MP3/FLAC/WAV)" | 补 AAC (仅按扩展名) | `set_format_from_path` 可输出 "AAC" |
| 31 | `main/audio/audio.h:54` | `get_position "当前解码位置(字节)"` | 补"FLAC 为文件读游标, 含预读" | `flac_get_position` 返回 `ftell` |
| 32 | `main/audio/pcm_pipeline.h:15` | "任意位深(8/16/24/32)" | 注明"上混路径支持 8/16/24/32；直通路径假设 16bit（当前解码器均 16bit）" | 直通分支不换位深 |
| 33 | `main/audio/pcm_pipeline.cpp:19/48` | "低于此值会溢出"/"32bit 兼容" | 澄清真实溢出边界 ≈6202Hz(取 8k)；缓冲按 32bit 预留但实际 16bit | 1152×44100/8192≈6202 |
| 34 | `main/storage/music_scan.c:318` | "legacy sdcard*.txt" | "文件名以 sdcard 开头的 legacy 文件" | 代码只判前缀，无扩展名 |
| 35 | `main/storage/music_scan.c:339` | 重复两行注释 | 合并为一行 | 上一轮修复遗留 |
| 36 | `main/storage/sys_monitor.c:178` | "打印文件数 + 容量信息" | "统计普通文件数 (仅计数, 未输出) + 打印容量" | `count` 只自增未输出 |
| 37 | `main/storage/sys_monitor.c:49` | "×50ms = 1s" | "标称 1s; 采样本身另耗 ~200ms" | `sample_sensors` 阻塞 ~200ms |
| 38 | `main/storage/sys_monitor.h:25` | group "如 sdcard 或 sdcard_xxx" | "真实 '/' 如 sdcard 或 sdcard/子目录" | 编码已改 '/' |
| 39 | `main/storage/sys_monitor.h:45` | "如 mount/unmount" | `"mounted"/"unmounted"/"mount_failed"` | 实际事件串 |
| 40 | `main/ui/ui_player.c:71` | `BRI_ANIM_OUT_MS "收回: 线性"` | "收回: ease_in (先慢后快)" | 代码 `lv_anim_path_ease_in` |
| 41 | `main/ui/menu/menu_bt.c:348` | "若列表空可再扫一轮" | "列表开着且未连接时继续下一轮扫描 (无列表空判断)" | `bt_maybe_start_scan` 只判开关/状态 |
| 42 | `main/app/console.c:20` | `SERIAL_TAG` 宏 | **删除**（全工程无引用） | grep 仅定义处 |
| 43 | `main/audio/cover.cpp:66/84` | 回调源行步进用 `iWidthUsed` | **代码改为 `pDraw->iWidth`**；拷贝宽度仍用 `iWidthUsed` | JPEGDEC.c:5363 `jd.iWidth=jd.iWidthUsed=iPitch`，仅 iWidthUsed 被右边缘裁小；内部缓冲行跨度=iWidth。原写法致右边缘块横向错位 |

### 本轮补全的“缺失函数/变量注释”
`ui_player.c`（约 20 个函数 + 删除流程/音量键状态变量）、`menu_bt.c`（4 回调 + 2 变量）、
`sys_monitor.c`（`sd_request_delete_file`/`init`/`deinit`/`sd_fs_unlock`/`is_mounted`/任务）、
`bt_a2dp.c`（`data_cb`/诊断静态量/`is_connected`）、`decoder_flac.cpp` 与 `decoder_wav.cpp` 的全部访问器、
`music_scan.c` 的 `NUM_EXTENSIONS`。均已按调用链确证后补写。

---

## 二、待确认清单（未证实，暂未改动）

> 已解消：原 #1（micro-flac 左对齐 int32）经 `managed_components/esphome__micro-flac/include/micro_flac/flac_decoder.h:291`
> 证实 `int32_t*` 重载即 "32-bit left-justified (MSB-aligned)"，且 `samples_decoded` 为所有声道交错样本数 —— **假设正确**。

| # | 文件:行 | 符号/代码 | 疑问 | 说明 |
|---|---------|-----------|------|------|
| 2 | `main/audio/decoder_wav.cpp` `fmt ` 分支 | 奇数 size 未跳 RIFF pad | 理论上可致后续块头错位 | 代码问题，非注释；影响极小，需真机样本验证后再改 |
| 3 | `main/audio/decoder_mp3.cpp:92-96` | APIC 描述串 `while(*p&&p<d+fs)p++;` | 先解引用后判界，畸形标签可能越界读 1 字节 | 影响极小（后续 isz 尺寸校验拦截） |
| 4 | `main/audio/resampler_fxp.cpp` | `decim` 直接丢帧、无前置抗混叠 | 高采样率音质风险 | 设计取舍，需听感验证；非注释错误 |
| 5 | `main/audio/resampler_fxp.cpp` | `taps=32`（q≥2p）分支 | 常规采样率下是否可达 | 分析表明偶数率经 decim 后 q<2p，32 抽头实际不可达；注释未声称必然使用，暂不改 |

### 已查证结论（附证据）
- **micro-flac**：`flac_decoder.h` 证实 `int32_t*` 输出为左对齐、`samples_decoded` 为交错样本数、`get_output_buffer_size_samples=max_block×channels`；`decoder_flac.cpp` 的 `out_cap`/`out_remaining`/移位逻辑均与之一致。
- **micro-mp3**：`mp3_decoder.h` 证实 `samples_decoded` 为**每声道**样本数、`get_bytes_per_sample()=2`、`MP3_MIN_OUTPUT_BUFFER_BYTES=4608`；`decoder_mp3.cpp` 的 `samples*channels*2` 与 4608 缓冲匹配。
- **jpegdec**：`JPEGDEC.c:5363/5376` 证实 draw 回调块行跨度为 `iWidth`，`iWidthUsed` 仅用于右边缘裁剪（已据此修正 `cover.cpp`）。

---
*本表由注释补全工作维护。已完成：第一轮 9 项 + 第二轮 34 项 = 修正/补全 43 项；待确认 4 项。*

---

## 三、网络 OTA（0.7.8）验证出的两条

| # | 结论 | 依据 |
|---|------|------|
| 1 | **`esp_ota_begin(part, image_size)` 会先把整包擦掉**：给了具体 size（而不是 `OTA_WITH_SEQUENTIAL_WRITES`）时，它直接 `esp_partition_erase_range(0, ALIGN_UP(size, 4K))`。2.2MB 就是十几秒不可中断的连续擦除，期间 cache 关着、别的任务全停。写 OTA 要用 sequential，让擦除跟着每次 write 只擦本次跨到的扇区。 | `components/app_update/esp_ota_ops.c:190`（预擦）与 `:314`（`need_erase` 分步擦） |
| 2 | **PSRAM 源写 flash 会退化成 32 字节一段**：ESP32 主 flash（SPI1）的 `supports_direct_write()` 恒为 false，于是 `esp_flash_write` 的 `direct_write` 为 false，按 `uint32_t temp_buf[8]`（**32 字节**）一段段搬到栈上再写（`memcpy` 在关 cache **之前**做，所以读 PSRAM 是安全的，只是慢）。原本为此备了 4KB 内部弹跳缓冲想走 direct 路径，**0.7.13 起删掉**：下载期间 8bit 内部堆只剩 2~3KB，4KB 申请不到、2KB 又会饿死 WiFi 的 1.6KB 收包缓冲，于是选择"慢一点、但不占内部 RAM"。 | `components/spi_flash/esp_flash_api.c:1128`/`:1152-1153` + `components/hal/spi_flash_hal.c:155`（`spi != SPI1_HOST` 才 true） |

另：`sdkconfig.defaults` 里"网络升级不做 TLS（会拉进 mbedtls + 60KB 证书包）"的旧结论在 0.7.8 作废 ——
只 pin 自己的 CA（`main/certs/ota_ca.pem`）就不用证书包，实测 TLS 全栈代价 +177,264 B，
app 槽还剩 2%（47,312 B）。

---

## 四、OTA 提速（0.7.19~0.7.22）验证出的四条

| # | 结论 | 依据 |
|---|------|------|
| 1 | **蓝牙控制器可以拆 host，但不能再 init 控制器**。`esp_bt_controller_disable()+deinit()+init()` 会打 `Coex register Wi-Fi channel change btdm cb faild`，随后 `assert failed: hci_hal_env_init hci_hal_h4.c:144 (event != NULL)` —— 开机时 `esp_bt_controller_mem_release(ESP_BT_MODE_BLE)` 已把 BLE 控制器内存还给堆，WiFi/lwIP 占走后，控制器重建时 `osi_event_create()` 分配失败 → assert → 反复重启。0.7.21 踩过，0.7.22 回退为"只拆 host"（实测还回 12.2~17.4 KB，装回 ~69 ms）。 | `components/bt/host/bluedroid/hci/hci_hal_h4.c:143-144` |
| 2 | **默认 `TCP_WND=5760` 只有 4 个 MSS**，是 OTA 下载的硬天花板；官方吞吐预设是 65534。我们只抬到 23040（16 MSS），并同步抬 `TCP_RECVMBOX_SIZE`。 | `components/wifi/iperf/sdkconfig.defaults.esp32`（IDF 官方"Improve Network Speed"指定的配置） |
| 3 | **收包缓冲按"省内存"压到 4 静态 / 2 动态 / RX_BA_WIN=2 会让 2.3MB 下载退化到 1KB/s**：`wstatt` 的 BUFFER 表里 `fail_fc=248`（流控失败 = 没有空闲收包缓冲），设备 190 秒只收到 262 个数据帧，服务器 `sendall` 一直阻塞（对端窗口关着）。0.7.22 起抬到 8/4/3（+约 11 KB，钱由蓝牙 host 让位出）。注意 `RX_BA_WIN ≤ DYNAMIC_RX_BUFFER_NUM`，Kconfig 有 `#error` 检查。 | `wstatt` 实测 + `wifi_init.c` 的 Kconfig 约束 |
| 4 | **大块暂存会放大"写 flash 停顿"**：收满 256 KB 才写，写完才继续收，期间 TCP 窗口关死、服务器 `sendall` 干等 —— 实测 2.3 MB 升级在 256 KB 边界反复卡 30~120 秒。0.7.21 起改为 64 KB 一段、写步长 16 KB、写:歇 = 9:1（90% 给烧写）。 | 服务器端 `[send]` 分块日志 + `update.c` 的 `UPD_STAGE_BYTES` |

另：手机热点（PC 与 ESP32 都是它的客户端）不适合做 OTA 测速 —— 客户端间转发实测下行 0.9 KB/s、
下载中 ping 丢包 92%；换普通路由器后同一份固件首段能跑到 100 KB/s 量级。测速请用路由器。

---

## 五、把 WiFi 的分配搬进私有堆（0.7.23）验证出的五条

| # | 结论 | 依据 |
|---|------|------|
| 1 | **WiFi 没有"传入缓冲区"的接口，只有传入分配器表的接口**：`wifi_init_config_t.osi_funcs` 是唯一入口，表里通用分配只有 `_malloc` / `_free` 两项，`_*_internal` 是"必须内部 RAM"的 DMA 那一组，`_wifi_*` 是允许去 PSRAM 的那一组。库里的 `os_calloc/os_zalloc/os_realloc` 建在这两个入口上。 | `esp_wifi.h:98/187/317` + `esp_private/wifi_os_adapter.h:73-129` |
| 2 | **预编译 WiFi 库的分配几乎全部走这张表**：`nm` 扫 7 个 `.a`，`U malloc/calloc/realloc` 一个都没有，仅 `libpp.a` 有 1 处 `U free`。所以换掉 `_malloc`/`_free` 就能覆盖 WiFi 的堆使用。 | `xtensa-esp32-elf-nm` 扫 `components/esp_wifi/lib/esp32/*.a` |
| 3 | **必须拦 libc `free`**：那唯一一处 `U free` 在 `libpp.a` 的 `hal_crypto.o`（`hal_crypto_set_key_entry` 里 `callx8 free`），它绕过 osi 表。原生 IDF 里 `_free` 就是 libc `free`，所以库作者这么写没问题；私有堆指针落到这里会被判非法释放。解法是 `-Wl,--wrap=free` + `__wrap_free` 按地址分流，不改 IDF。实测链接后镜像里 `U free` 归零，`__wrap_free` 被引用 171 处。 | `objdump` hal_crypto.o 反汇编 + 链接后 `nm music_nano.elf` |
| 4 | **`multi_heap_register` 的 tlsf 元数据约 3.2 KB（不随区域变大而变）**，33,024 B 的 LVGL 绘制缓冲建堆后实际可用约 29.7 KB，所以"33KB 私有堆"要按 ~29.7 KB 算。IDF 自己的测试也用 `HEAP_OVERHEAD_MAX = 3248` 作为上界。 | `multi_heap.h:98` + `test_apps/heap_tests/main/test_runtime_heap_reg.c:26` + 实测 `multi_heap_free_size()` |
| 5 | **两块绘制缓冲在最终镜像里相邻且 4 字节对齐**（`s_draw_buf2` 0x3ffc54b4 / `s_draw_buf1` 0x3ffcd5b4，差 0x8100 = 33,024），所以在需要更大私有堆时可以把 buf1 尾部与 buf2 合并成一段连续区域。注意必须给数组加 `__attribute__((aligned(4)))`：16 位色深下 `lv_color_t` 只有 2 字节，默认对齐不足以让 multi_heap 按 `size_t` 读写块头。 | `nm music_nano.elf` 符号地址差 |

---

## 六、OTA 提速收尾（0.7.9~0.7.12）验证出的五条

| # | 结论 | 依据 |
|---|------|------|
| 1 | **下载慢的真因是 RX 聚合窗口，不是收包缓冲不够**。`RX_BA_WIN=3` 对上 AP 协商的 64 帧窗口（日志 `ba-add ... winSize:64`），40MHz 下一个聚合组轻松超 3 帧，超出的直接丢 → TCP 重传 → 20~32 秒退避。改成 `RX_BA_WIN=8`（同时 `DYNAMIC_RX_BUFFER_NUM 4→8`，Kconfig 要求窗口 ≤ 动态缓冲数）后：2.0~2.8 KB/s → **10.8 KB/s**，最长停滞 32 s → 2.5 s。**把 DMA 从 4.7KB 腾到 19.7KB 反而没提速**（2.0 vs 3.4 KB/s，纯噪声）—— 内存这条路当时是伪线索。 | 两轮实测对比 + `esp_wifi.h` 的 `rx_ba_win` |
| 2 | **写 flash 的 1.22 s/16KB 拆开是"编程 545ms + 擦 70ms + 22ms 开销"，而 PSRAM 源会让编程翻倍到 1130ms**。`esp_flash_write` 的 `direct_write` 只看源在不在 DRAM（`esp_flash_api.c:1128`），PSRAM 源退化成 32 字节一段、每段关一次 cache。改成 4KB 内部中转后单步 1.22s → **0.62s**，2.39MB 写阶段 183s → 93s。 | `flashspd` 命令实测（每项带回读校验）：C 545,644us / C2 1,130,545us / C3 548,303us / D 617,260us = C+A / E 545,680us = C |
| 3 | **64KB 块擦真的是 17.6ms**（`0xFF` 回读通过），比 4 个 4KB 扇区擦的 70.7ms 便宜 4 倍 —— 但它只占 16KB 写耗时的 11%，换 `esp_ota_resume` + 滚动块擦只能再省 8.5%（93s → 85s），不值得接管擦除时序。 | `flashspd` 的 A/B 项 + `esp_flash_api.c:739`（地址/长度按块对齐才走块擦） |
| 4 | **私有堆三组分配全部改成"私有堆优先、满了落公共堆"的两级**：只用私有堆时 99% 满那一刻有 68 次 malloc 拿到 NULL；两级后 3,462 次分配 **0 失败、332 次溢出**，两边的池子都用上了。释放不需要区分，`wifi_arena_free` 按地址分流。 | 0.7.32 / 0.7.33 两次完整 OTA 的 `私有堆关闭` 日志 |
| 5 | **`esp_ota_end` 校验需要空闲 MMU 页，现在一页都没有 → 镜像校验必失败**（`esp_image: No free MMU pages are available`），所以下载和写入都成功后仍然切不了槽。2MB PSRAM 占掉大半 DROM 页，剩下的被 IROM/DROM 静态映射吃完；镜像 `.rodata` 段 828KB 要 13 页。这是 OTA 最后一个拦路虎，未解。 | `esp_image_format.c:662-686`（`bootloader_mmap_get_free_pages()` 为 0 就报错）+ 实测 2.39MB 全量下载 |