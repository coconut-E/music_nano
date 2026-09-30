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
