#!/usr/bin/env python3
"""局域网 OTA 调试服务器 —— 伪装成 GitHub Releases 的那两个接口

设备端只有一条代码路径 (认 GitHub 的形状). 这个脚本不发明新协议, 只做两件事:

    GET /repos/<owner>/<repo>/releases?per_page=N   → 和 GitHub 一样的 JSON 数组
    GET /releases/download/<tag>/<asset>            → 固件二进制 (Content-Length)

于是"局域网调试 → 真服务器"只是改设备里的两个字符串 (串口 `ota server ...`),
不用改一行固件代码, 也不会出现"局域网能升、上线不能升"这种两套逻辑分叉.

用法:
    python tools/ota_server.py --bin build/music_nano.bin --version 0.7.9
    python tools/ota_server.py --dir build/ota            # 目录里放多个 music_nano x.y.z.bin
    python tools/ota_server.py --bin ... --http           # 不开 TLS, 只验 JSON 用
    python tools/ota_server.py --bin ... --version 0.7.9 --slice-kb 256
                                                          # 只发前 256KB, 测速率用
                                                          # (速率几 KB/s 时不用等整包)

广播的 IP: 默认优先取 Windows「移动热点」那张网卡 (192.168.137.x) —— 走热点时 PC 自己
就是 AP, 设备直连, 中间没有转发; 拿不到热点才退回默认路由出口 IP. 想指定就 --ip.

证书: 不需要先跑 ota_cert.ps1. 服务器启动时自检 SAN, 缺哪个 IP 就自动调
      tools/ota_cert.ps1 -Ip "<旧SAN>,<新IP>" 补进去 (带 -ExecutionPolicy Bypass,
      免得被"脚本未签名"挡住). CA 不动, 所以设备端 pin 的 CA 不用重编固件.
      不想自动签就 --no-sign.

注意: 这是**调试**服务器, 只处理 GET, 没有并发压力, 也没做任何鉴权.
"""

import argparse
import hashlib
import json
import os
import re
import socket
import ssl
import subprocess
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import quote, unquote, urlparse

# 发布约定: music_nano 0.7.9.bin (空格; 下划线/短横也认, 和固件里的匹配规则一致)
ASSET_RE = re.compile(r"^music_nano[ _-](\d+\.\d+\.\d+)\.bin$")


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def collect(bin_path, dir_path, version):
    """收集要"发布"的固件: [(version, path, asset_name), ...]"""
    items = []
    if bin_path:
        name = os.path.basename(bin_path)
        m = ASSET_RE.match(name)
        if not m:
            # 允许直接指 build/music_nano.bin: 用 --version 起一个合规的资产名
            name = "music_nano %s.bin" % version
            m = ASSET_RE.match(name)
        items.append((m.group(1), bin_path, name))
    if dir_path:
        for f in sorted(os.listdir(dir_path)):
            m = ASSET_RE.match(f)
            if m:
                items.append((m.group(1), os.path.join(dir_path, f), f))

    seen, out = set(), []
    for ver, path, name in items:
        if ver in seen or not os.path.isfile(path):
            continue
        seen.add(ver)
        out.append((ver, path, name))
    if not out:
        sys.exit("没有可发布的固件 (--bin 或 --dir)")
    out.sort(key=lambda t: [int(x) for x in t[0].split(".")])
    return out


def cert_san_ips(cert_path):
    """读证书里的 IP 列表 (subjectAltName 的 IP 项).

    用 CPython 自带的 ssl._ssl._test_decode_cert, 不需要装 cryptography.
    读不出来就返回 None, 由调用方决定放不放行.
    """
    try:
        info = ssl._ssl._test_decode_cert(cert_path)
    except Exception:
        return None
    out = []
    for kind, val in info.get("subjectAltName", ()):
        if kind in ("IP Address", "IP"):
            out.append(val)
    return out


def guess_lan_ip():
    """不真的发包, 只是让内核选一条路由, 拿到本机出口 IP"""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("10.255.255.255", 1))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except OSError:
        return "127.0.0.1"


# Windows「移动热点」的网段: ICS 固定把 192.168.137.1 给本机, 客户端 .2 往后.
# 走热点时 PC 自己就是 AP, ESP32 直连过来, 中间没有第二层转发 —— 之前用手机热点
# (PC 和设备都是手机的客户端) 时, 客户端间转发实测只有 0.9 KB/s, 下载会被它卡死.
ICS_PREFIX = "192.168.137."


def hotspot_ip():
    """找 Windows 移动热点网卡的地址; 没有热点就返回 None.

    只认 192.168.137.x 这一段, 不依赖 ipconfig 的中文/英文措辞 (各语言版本的字段名
    都不一样), 只从输出里捞 IPv4 形状的数字再按网段筛.
    """
    try:
        out = subprocess.run(["ipconfig"], capture_output=True, text=True,
                             encoding="mbcs", errors="ignore", timeout=5).stdout
    except Exception:
        return None
    for ip in re.findall(r"\b(?:\d{1,3}\.){3}\d{1,3}\b", out):
        if ip.startswith(ICS_PREFIX) and not ip.endswith((".0", ".255")):
            return ip
    return None


def auto_sign(cert_path, ip):
    """把 ip 补进证书 SAN (CA 不动, 所以设备端 pin 的 CA 不用重编).

    Windows 默认执行策略禁止运行未签名脚本, 直接 .\\tools\\ota_cert.ps1 会报
    "未对文件进行数字签名" —— 所以这里显式带 -ExecutionPolicy Bypass.
    重签用的是"旧 SAN + 新 IP"的并集, 换回原来的网段就不用再签一次.
    """
    script = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ota_cert.ps1")
    ca_key = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ota_cert", "ca.key")
    if not os.path.isfile(script):
        print("!! 找不到 %s, 不能自动重签" % script)
        return False
    if not os.path.isfile(ca_key):
        # 没有 CA 时脚本会顺手生成一张新的, 那样设备端 pin 的 CA 就对不上了
        print("!! tools/ota_cert/ca.key 不存在, 自动重签会换掉 CA (固件要跟着重编)")
        print("   先手动跑一次: powershell -ExecutionPolicy Bypass -File tools\\ota_cert.ps1 -Ip %s" % ip)
        return False

    old = cert_san_ips(cert_path) or []
    ips = []
    for a in list(old) + [ip]:
        if a and a not in ips:
            ips.append(a)

    cmd = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
           "-File", script, "-Ip", ",".join(ips)]
    print(" 证书里没有 %s, 自动重签 (CA 不动):" % ip)
    print("   %s" % " ".join(cmd))
    try:
        # 脚本输出是 UTF-8 (本机 pwsh 的 OutputEncoding), 用 text=True 会按 GBK 解码
        # 直接抛 UnicodeDecodeError —— 显式指定, 解不出来的字节替换掉即可.
        r = subprocess.run(cmd, capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=120)
    except Exception as e:
        print("!! 重签失败: %s" % e)
        return False
    if r.returncode != 0:
        print("!! 重签失败 (exit %d)" % r.returncode)
        for line in (r.stdout + r.stderr).strip().splitlines()[-12:]:
            print("   " + line)
        return False
    return os.path.isfile(cert_path) and ip in (cert_san_ips(cert_path) or [])


class Handler(BaseHTTPRequestHandler):
    server_version = "ota-mock/1.0"
    protocol_version = "HTTP/1.1"        # 让 Content-Length 生效、连接可复用

    def log_message(self, fmt, *args):
        sys.stderr.write("[%s] %s\n" % (self.log_date_time_string(), fmt % args))

    # ── 路由 ──
    def do_GET(self):
        path = urlparse(self.path).path
        if "/download/" in path:
            self.serve_asset(path)
        elif path.rstrip("/").endswith("/releases"):
            self.serve_json()
        else:
            self.serve_index()

    def do_HEAD(self):
        # 设备不发 HEAD; 这个是给 curl -I 自检用的 (只回头, 不发 body)
        self.do_GET()

    def send_body(self, code, ctype, body):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.write_body_paced(body)

    # 分块发送并打进度.
    #
    # 为什么分块: 一次 sendall(2.3MB) 只会告诉我们"最后终于发完了", 分不清是
    #   服务器发不出去, 还是客户端收得慢. 每 64KB 打一行 (已发/耗时/瞬时速率),
    #   就能看出:
    #     - 前几行飞快、之后每行都等 10 秒  → 对端窗口关着, 是设备侧收不动;
    #     - 全程都匀速地慢                → 路径/AP 在限速或丢包.
    # 注意 wfile 是 _SocketWriter (无缓冲), 所以每次 write 都阻塞到内核收下.
    CHUNK = 64 * 1024

    def write_body_paced(self, body):
        total = len(body)
        t0 = time.monotonic()
        sent = 0
        while sent < total:
            self.wfile.write(body[sent:sent + self.CHUNK])
            sent += min(self.CHUNK, total - sent)
            el = time.monotonic() - t0
            sys.stderr.write("[send] %d/%d B, %.1f s, %.1f KB/s\n"
                             % (sent, total, el,
                                sent / 1024.0 / el if el > 0 else 0.0))
        el = time.monotonic() - t0
        sys.stderr.write("[send] 完成 %d B, 用时 %.1f s, 平均 %.1f KB/s\n"
                         % (total, el, total / 1024.0 / el if el > 0 else 0.0))

    # ── 版本接口 ──
    def serve_json(self):
        base = self.server.base_url
        rels = []
        for ver, fpath, name in self.server.items:
            data = self.server.sliced.get(ver)
            size = len(data) if data is not None else os.path.getsize(fpath)
            digest = hashlib.sha256(data).hexdigest() if data is not None else sha256_of(fpath)
            rels.append({
                "tag_name": "v" + ver,
                "name": "v" + ver,
                "draft": False,
                "prerelease": False,
                "created_at": "2026-01-01T00:00:00Z",
                "body": "LAN mock release (tools/ota_server.py)",
                "assets": [{
                    "name": name,
                    "size": size,
                    # digest 放在 size 和 browser_download_url 之间:
                    # 设备端的解析器是在这个区间里往前找的 (GitHub 也是这个顺序)
                    "digest": "sha256:" + digest,
                    "browser_download_url": "%s/releases/download/v%s/%s"
                                            % (base, ver, quote(name)),
                }],
            })
        body = json.dumps(rels, ensure_ascii=False, indent=2).encode("utf-8")
        self.send_body(200, "application/json; charset=utf-8", body)

    # ── 固件下载 ──
    def serve_asset(self, path):
        name = unquote(path.rsplit("/", 1)[-1])
        m = ASSET_RE.match(name)
        if not m:
            self.plain(404, "bad asset name: %s" % name)
            return
        ver = m.group(1)
        for v, fpath, n in self.server.items:
            if v == ver:
                data = self.server.sliced.get(ver)
                if data is None:
                    with open(fpath, "rb") as f:
                        data = f.read()
                self.send_body(200, "application/octet-stream", data)
                return
        self.plain(404, "no such version: %s" % ver)

    def serve_index(self):
        lines = ["ota mock server", "", "JSON: " + self.server.api_url]
        for ver, fpath, name in self.server.items:
            lines.append("bin : %s/releases/download/v%s/%s"
                         % (self.server.base_url, ver, quote(name)))
        self.plain(200, "\n".join(lines) + "\n")

    def plain(self, code, text):
        self.send_body(code, "text/plain; charset=utf-8", text.encode("utf-8"))


def main():
    ap = argparse.ArgumentParser(description="局域网 OTA 调试服务器 (伪装 GitHub Releases)")
    ap.add_argument("--bin", help="要发布的固件 (build/music_nano.bin)")
    ap.add_argument("--dir", help="目录里所有 music_nano x.y.z.bin 都发布 (用来测「选最高版本」)")
    ap.add_argument("--version", default="0.0.0", help="--bin 的文件名不合规时, 用它当版本")
    ap.add_argument("--port", type=int, default=8443)
    ap.add_argument("--ip", help="对外公布的 IP (默认: Windows 热点 192.168.137.x, 没有就用默认路由)")
    ap.add_argument("--cert", default="tools/ota_cert/server.crt")
    ap.add_argument("--key", default="tools/ota_cert/server.key")
    ap.add_argument("--http", action="store_true", help="不开 TLS (只验 JSON 用)")
    ap.add_argument("--no-sign", action="store_true", help="证书里没有这个 IP 时不自动重签, 只报错")
    ap.add_argument("--owner", default="coconut-E")
    ap.add_argument("--repo", default="music_nano")
    ap.add_argument("--slice-kb", type=int, default=0,
                    help="只发每个固件的前 N KB (测速用: 1~5KB/s 时不用等整包 8 分钟)")
    args = ap.parse_args()

    items = collect(args.bin, args.dir, args.version)

    # 切片: 速率只有几 KB/s 时, 2.4MB 整包要等 8 分钟, 一轮测试根本走不完.
    # 发前 N KB 就能在 1 分钟内看到稳态速率. 代价是镜像不完整, 设备最后校验会失败
    # (槽位不会切, 也不会变砖), 但"下载阶段"的数字是真的.
    sliced = {}
    if args.slice_kb > 0:
        n = args.slice_kb * 1024
        for ver, fpath, name in items:
            with open(fpath, "rb") as f:
                sliced[ver] = f.read(n)

    # 优先用 Windows 热点那张网卡: 设备直连热点时, PC 就是 AP, 中间没有转发.
    # 拿不到热点才退回"默认路由出口 IP"(有线/路由器/WiFi 直连).
    hs = None if args.ip else hotspot_ip()
    if args.ip:
        ip, src = args.ip, "--ip 指定"
    elif hs:
        ip, src = hs, "Windows 移动热点"
    else:
        ip, src = guess_lan_ip(), "默认路由 (没找到热点网段 192.168.137.x)"
    print(" 广播 IP: %s (%s)" % (ip, src))
    if not args.ip and not hs:
        print(" 想走热点: 先打开 Windows「移动热点」再跑本脚本, 或直接 --ip 192.168.137.1")

    scheme = "http" if args.http else "https"
    base = "%s://%s:%d" % (scheme, ip, args.port)

    srv = ThreadingHTTPServer(("0.0.0.0", args.port), Handler)
    srv.items = items
    srv.sliced = sliced
    srv.base_url = base
    srv.api_url = "%s/repos/%s/%s/releases?per_page=5" % (base, args.owner, args.repo)

    if not args.http:
        if not (os.path.isfile(args.cert) and os.path.isfile(args.key)):
            sys.exit("证书不存在: %s / %s —— 先跑 tools\\ota_cert.ps1" % (args.cert, args.key))

        # 启动自检: 证书里必须有"待会儿要广播给设备的那张网卡 IP".
        # 不做这个检查的后果很隐蔽: 服务器一切正常(浏览器能开、curl 能通), 设备却在
        # TLS 握手时报 mbedtls_ssl_handshake returned -0x2700, 而且服务器日志里
        # 一条请求都没有 —— 因为握手阶段就断了, 请求根本没到 HTTP 层.
        san = cert_san_ips(args.cert)
        if san is None:
            print("!! 读不出 %s 的 SAN, 跳过 IP 自检" % args.cert)
        elif ip not in san:
            print("=" * 66)
            print(" 证书里没有要广播的 IP: %s" % ip)
            print("=" * 66)
            print("  现有 SAN: %s" % ", ".join(san))
            print("  不修的话设备端会报: mbedtls_ssl_handshake returned -0x2700")
            print("  这边日志里一条请求都不会有 —— TLS 握手就断了")
            print("=" * 66)
            if args.no_sign:
                print("  --no-sign: 不自动重签, 退出")
                print("  手动: powershell -ExecutionPolicy Bypass -File tools\\ota_cert.ps1 -Ip %s" % ip)
                sys.exit(2)
            if not auto_sign(args.cert, ip):
                print("!! 自动重签失败, 退出 (原因见上)")
                sys.exit(2)
            print(" 重签完成, SAN: %s" % ", ".join(cert_san_ips(args.cert) or []))

        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(args.cert, args.key)
        srv.socket = ctx.wrap_socket(srv.socket, server_side=True)

    print("=" * 66)
    print(" OTA 调试服务器  %s" % base)
    print("=" * 66)
    for ver, fpath, name in items:
        n = len(sliced[ver]) if ver in sliced else os.path.getsize(fpath)
        print("  发布: %-28s %8d B  (v%s)%s"
              % (name, n, ver, "  [切片]" if ver in sliced else ""))
    print()
    print(" 版本接口: %s" % srv.api_url)
    print(" 自检    : curl -k \"%s\"" % srv.api_url)
    print()
    print(" 设备端串口输入 (一行为一条命令):")
    print("   ota server %s" % srv.api_url)
    print(" 回到 GitHub:")
    print("   ota reset")
    print("=" * 66)

    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nbyebye")


if __name__ == "__main__":
    main()
