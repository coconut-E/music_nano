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

证书: 先跑 tools/ota_cert.ps1 (生成 tools/ota_cert/server.crt / server.key,
      SAN 带当前局域网 IP; CA 已经拼进固件的 main/certs/ota_ca.pem).

注意: 这是**调试**服务器, 只处理 GET, 没有并发压力, 也没做任何鉴权.
"""

import argparse
import hashlib
import json
import os
import re
import socket
import ssl
import sys
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
            self.wfile.write(body)

    # ── 版本接口 ──
    def serve_json(self):
        base = self.server.base_url
        rels = []
        for ver, fpath, name in self.server.items:
            rels.append({
                "tag_name": "v" + ver,
                "name": "v" + ver,
                "draft": False,
                "prerelease": False,
                "created_at": "2026-01-01T00:00:00Z",
                "body": "LAN mock release (tools/ota_server.py)",
                "assets": [{
                    "name": name,
                    "size": os.path.getsize(fpath),
                    # digest 放在 size 和 browser_download_url 之间:
                    # 设备端的解析器是在这个区间里往前找的 (GitHub 也是这个顺序)
                    "digest": "sha256:" + sha256_of(fpath),
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
    ap.add_argument("--ip", help="对外公布的 IP (默认自动探测)")
    ap.add_argument("--cert", default="tools/ota_cert/server.crt")
    ap.add_argument("--key", default="tools/ota_cert/server.key")
    ap.add_argument("--http", action="store_true", help="不开 TLS (只验 JSON 用)")
    ap.add_argument("--owner", default="coconut-E")
    ap.add_argument("--repo", default="music_nano")
    args = ap.parse_args()

    items = collect(args.bin, args.dir, args.version)
    ip = args.ip or guess_lan_ip()
    scheme = "http" if args.http else "https"
    base = "%s://%s:%d" % (scheme, ip, args.port)

    srv = ThreadingHTTPServer(("0.0.0.0", args.port), Handler)
    srv.items = items
    srv.base_url = base
    srv.api_url = "%s/repos/%s/%s/releases?per_page=5" % (base, args.owner, args.repo)

    if not args.http:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(args.cert, args.key)
        srv.socket = ctx.wrap_socket(srv.socket, server_side=True)

    print("=" * 66)
    print(" OTA 调试服务器  %s" % base)
    print("=" * 66)
    for ver, fpath, name in items:
        print("  发布: %-28s %8d B  (v%s)" % (name, os.path.getsize(fpath), ver))
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
