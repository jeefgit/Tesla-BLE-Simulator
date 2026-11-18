#!/usr/bin/env python3
"""USB OTA 本地页面服务器。

Web Serial 只在安全上下文可用（https 或 localhost），所以不要直接双击
index.html（file:// 下浏览器不会提供 navigator.serial），用本脚本起一个
仅监听 127.0.0.1 的静态服务器，并自动打开浏览器。
"""

from __future__ import annotations

import functools
import http.server
import os
import socketserver
import threading
import webbrowser

HOST = "127.0.0.1"
PORT = 8787
ROOT = os.path.dirname(os.path.abspath(__file__))


class Handler(http.server.SimpleHTTPRequestHandler):
    # 静态页面访问日志没有价值，保持控制台干净。
    def log_message(self, fmt: str, *args: object) -> None:
        pass

    def end_headers(self) -> None:
        # 升级期间避免浏览器缓存旧的 index.html。
        self.send_header("Cache-Control", "no-store")
        super().end_headers()


def main() -> None:
    handler = functools.partial(Handler, directory=ROOT)
    with socketserver.TCPServer((HOST, PORT), handler) as httpd:
        url = f"http://localhost:{PORT}/index.html"
        print(f"USB OTA 页面: {url}")
        print("按 Ctrl+C 停止。")
        threading.Timer(0.4, lambda: webbrowser.open(url)).start()
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\n已停止。")


if __name__ == "__main__":
    main()
