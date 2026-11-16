# 固件更新 (OTA) 测试记录

> 现状：**协议 v0，不保留任何兼容层**。这个项目还没有已发布的旧设备，所以接口
> 可以随时改，改了固件和服务器两边一起跟。
>
> 服务器端代码不在本仓库，本文件只记录固件侧的行为、接口形状和实测数据。

---

## 1. 一张图

```
设备 ──HTTPS──► Cloudflare 边缘 ──出站隧道──► 手机 (Termux) ──► ota_server.py
      查版本                                                      │
                                                                  │ 需要时把
设备 ──HTTP───► 同一域名 80 端口 ────────────────────────────────┘ firmware.bin
      拉 10 个压缩块                                              切成 10 块
```

- **查版本走 HTTPS**，**下载走 HTTP**。这不是疏忽：
  - HTTPS 那一步只拉 400 字节清单，TLS 握手没问题；
  - 下载那一步如果走 TLS，握手会把内部 DMA 池从 8.7KB 打碎到 **1.4KB**，
    比 WiFi 收一帧（1.6KB）还小 —— 实测设备会彻底收不到包，20 秒超时一次接一次。
  - 明文下载的代价是内容可被中间人替换。权衡后接受：爱好者项目、代码开源。
    要做的话正确解法是给固件加 Ed25519 签名，不是把下载塞回 TLS。

## 2. 接口（v0，只有一种形状）

```
GET https://coconute.dpdns.org/ota/v1/manifest.json?channel=stable&device=<MAC>
→ 200 {
    "schema": 1,
    "channel": "stable",
    "version": "0.8.7",          ← 按段比较, 比本机大才升
    "size": 2396416,             ← 解压后的整包长度
    "sha256": "…",               ← 解压后的整包哈希, 固件必校验
    "notes": "…",
    "released_at": "…",
    "chunks": [
      {"url": "http://…/fw/stable/0.8.7/c0.gz", "size": 55400, "raw": 239642},
      …10 项
    ]
  }
→ 404  该频道没有发布
```

**没有** `url` 字段，也**没有**“整包直接下”的路径 —— 固件只认 `chunks`，
清单里没有 `chunks` 就直接报 `UPD_E_PARSE`。

设备地址里的 `device=` 是 `esp_efuse_mac_get_default()` 读出的芯片号，服务器
只在响应里回显，作用是让手机上的 `ota.log` 能对出是哪台设备在问，不参与鉴权。

固件里的地址是编译期常量（`update.c` 的 `UPD_API`），**不存 NVS，也没有串口
命令能改**。换服务器 = 改那一行 + 重编一版。

## 3. 为什么分块 + 压缩

单块大小 = 固件大小 / 10，每块**独立**用 zlib 压缩：

- **必须每块独立压**：deflate 是有状态的，整包压成一个流之后，单独取其中任意
  一段都解不出东西。只有独立小流才能做到“某块失败只重下那一块”。
- **省 38%**：实测 0.8.6 整包 2,400,576 B → 压缩后 1,481,796 B（61.7%）。
  这条跨境链路只有 2~6 KB/s，38% 就是几分钟。
- **抗抖动**：单块约 150 KB，失败只重下这一块，最多重试 3 次。
  实测下午时段第一块掉到 0.24 KB/s、230 秒后超时，重试一次就正常跑完了。

注意服务器发的是 **zlib 包装（magic `78 9c` / `78 da`）而不是 gzip（`1f 8b`）**：
ESP32 ROM 里那份 miniz 只有 `TINFL_FLAG_PARSE_ZLIB_HEADER`，没有 gzip 头支持。
文件名虽然叫 `c0.gz`，内容其实是 zlib。

## 4. 发布一版

```powershell
# PC: 编出 build/music_nano.bin, 推给手机
adb push build\music_nano.bin /data/local/tmp/fw.bin

# 手机 Termux: 发布 (服务器每次请求都会重新扫描, 不用重启)
cd ~/server/ota
python build_release.py /data/local/tmp/fw.bin --version 0.8.7 --channel stable
```

分块文件由 `ota_server.py` 在**第一次有人请求清单时**自动生成并缓存；`firmware.bin`
的大小或 mtime 变了会重建。

⚠️ `--notes` 里不要带空格：通过 `adb shell` / `ssh` 传参时引号容易被吃掉。

## 5. 实测数据

| 场景 | 传输量 | 耗时 | 备注 |
|---|---|---|---|
| 整包 HTTPS（0.8.4） | 2.40 MB | 384 s | 凌晨 6 KB/s；同一配置下午再跑就 600 秒超时失败 |
| 整包 + 限速 10 KB/s | — | 失败 | **限速反而更慢**（0.4 KB/s）：慢速流每秒两三个包，丢一个就要等 RTO |
| 整包 + SACK/窗口调优 | 2.40 MB | 393 KB 处崩 | 内部 RAM 只剩 22 KB，窗口长期关死 |
| **10 块 zlib（0.8.6）** | **1.48 MB** | **372 s** | 10 块零重试，全程没有 20 秒超时 |
| 10 块 zlib（0.8.7，下午） | 1.48 MB | 首块 230 s 超时 → 重试后跑完 | 重试机制第一次真正生效 |

## 6. 踩过的坑

1. **`tinfl_decompress_mem_to_mem()` 不能用**
   它把 `tinfl_decompressor`（带 32 KB 字典窗口）放在**栈**上，8 KB 的 wifi
   任务当场 `A stack overflow in task wifi` panic。必须自己 `malloc` 到 PSRAM，
   循环外分配一次、每块 `tinfl_init` 重置。

2. **分块文件必须放在版本目录根下**
   下载路由只认 `/fw/<ch>/<ver>/<file>` 四段，放 `chunks/` 子目录会 404。

3. **Cloudflare 会缓存固件，绕过限速**
   原来给固件发 `Cache-Control: public, max-age=31536000, immutable`，CF 把
   `.bin` 缓存到边缘，之后设备直接从边缘拿（35 KB/s），源站的限速完全没用。
   现在固件响应是 `no-store`。

4. **下载不能走 TLS**（最重要的一条）
   TLS 握手会让内部 DMA 池从 8.7 KB 掉到 1.4 KB，最大连续块比 WiFi 一帧还小，
   于是收不到包、20 秒超时、最后放弃。查版本的 HTTPS 握手没这个问题——那是
   一次性小请求，握手完就释放了。

5. **限速是反效果**
   低速 TCP 流每秒只有两三个包，丢一个就要等 RTO（1s→2s→4s…），恢复比快流慢。
   实测 10 KB/s 限速下设备只有 0.4 KB/s，不限制反而有 5~6 KB/s。

## 7. 排查

设备串口看这几行就够判断卡在哪：

```
分块清单: 10 块                        ← 清单解析成功
开始分块下载: … B → … B (压缩 62%), 共 10 块
建连完成, 栈余量 … 内部 … DMA …        ← 看 DMA 最大块, 低于 1600 就危险
第 N/10 块失败 (-1), 2 秒后重试        ← -1 是读超时, 不是数据错
第 N/10 块完成: 已写 … (… KB/s)        ← 块内实际速率
写入完成: … B → ota_1, 耗时 … ms
```

`esp_ota_set_boot_partition` 只在最后一步调用，中途任何失败都不会动 otadata，
设备照常跑原固件。

## 8. 历史

本文原先记录的是 0.7.x 时代的 GitHub Releases + gh-proxy 方案（分区布局、字库
拆分区、证书 pin、`ota server` 串口命令）。那套东西在 0.8.7 全部移除：

- GitHub Releases / gh-proxy → 自建手机服务器
- 整包 Range 下载 → 10 块独立 zlib 压缩
- NVS 存服务器地址 + `ota server` / `ota reset` 命令 → 编译期常量
- 本地自签 CA 签证书 → 直接用 Cloudflare 边缘的 Let's Encrypt 证书
