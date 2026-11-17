#!/usr/bin/env python3
"""从 PlatformIO 串口日志里还原 ESP32 双击截图。

固件在双击屏幕后会把 466x466 的 BMP（24bit）转成 base64 打印到 USB 串口，
前后带 ===SHOT-BEGIN=== / ===SHOT-END=== 标记。本脚本从日志或标准输入里
提取所有截图，还原成 .bmp；加 --png 时用 Pillow 转成 PNG。

用法：
    pio device monitor -f log2file -f default     # 先连上串口，日志会写文件
    # 双击屏幕，等出现 ===SHOT-END===
    # Ctrl+C 退出
    python tools/decode_screenshot.py platformio-device-monitor-*.log
    python tools/decode_screenshot.py shot.log --png

也可以直接管道：
    pio device monitor | python tools/decode_screenshot.py -
"""

from __future__ import annotations

import argparse
import base64
import re
import sys
import time
from pathlib import Path

BEGIN = "===SHOT-BEGIN==="
END = "===SHOT-END==="
B64_CHARS = set(
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/="
)
META_RE = re.compile(r"w=(\d+)\s+h=(\d+)\s+bytes=(\d+)")


def extract(text: str):
    """返回 [(width, height, expected_bytes, payload_bytes), ...]。"""
    shots = []
    pos = 0
    while True:
        begin = text.find(BEGIN, pos)
        if begin < 0:
            break
        end = text.find(END, begin + len(BEGIN))
        if end < 0:
            print("警告：找到 SHOT-BEGIN 但没有 SHOT-END，日志可能被截断。",
                  file=sys.stderr)
            break

        block = text[begin + len(BEGIN):end]
        meta = META_RE.search(block)
        if meta is None:
            print("警告：SHOT-BEGIN 后没有 w/h/bytes 元信息，已跳过。",
                  file=sys.stderr)
            pos = end + len(END)
            continue

        width, height, expected = (int(g) for g in meta.groups())
        # base64 每行都带 SHOT: 前缀，只收集这些行；串口日志里的其他内容
        # （时间戳、ANSI 颜色码等）不会被混进数据。
        payload = "".join(
            line[5:] for line in block.splitlines() if line.startswith("SHOT:")
        )
        payload = "".join(ch for ch in payload if ch in B64_CHARS)
        try:
            data = base64.b64decode(payload, validate=False)
        except ValueError as exc:
            print(f"警告：第 {len(shots) + 1} 张截图 base64 解码失败：{exc}",
                  file=sys.stderr)
            pos = end + len(END)
            continue

        if len(data) != expected:
            print(f"警告：第 {len(shots) + 1} 张截图长度不符，"
                  f"期望 {expected} 字节，实际 {len(data)} 字节。",
                  file=sys.stderr)

        shots.append((width, height, expected, data))
        pos = end + len(END)
    return shots


def to_png(bmp_path: Path) -> Path:
    try:
        from PIL import Image
    except ImportError:
        print("提示：未安装 Pillow，跳过 PNG 转换（pip install pillow）。",
              file=sys.stderr)
        return bmp_path
    png_path = bmp_path.with_suffix(".png")
    with Image.open(bmp_path) as img:
        img.save(png_path)
    return png_path


def decode_bytes(raw: bytes) -> str:
    """兼容 Windows 重定向可能产生的 UTF-16 / UTF-8 BOM 日志。"""
    if raw.startswith((b"\xff\xfe", b"\xfe\xff")):
        return raw.decode("utf-16", errors="replace")
    if raw.startswith(b"\xef\xbb\xbf"):
        return raw.decode("utf-8-sig", errors="replace")
    if raw.count(b"\x00") > len(raw) // 4:
        return raw.decode("utf-16-le", errors="replace")
    return raw.decode("utf-8", errors="replace")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("log", nargs="?", default="-",
                        help="串口日志文件；- 表示从标准输入读取（默认）")
    parser.add_argument("-o", "--out", help="输出文件名（仅单张截图时有效）")
    parser.add_argument("--png", action="store_true", help="同时转换成 PNG（需要 Pillow）")
    parser.add_argument("--dir", default=".", help="输出目录（默认当前目录）")
    args = parser.parse_args()

    if args.log == "-":
        text = decode_bytes(sys.stdin.buffer.read())
    else:
        text = decode_bytes(Path(args.log).read_bytes())

    shots = extract(text)
    if not shots:
        print("没有找到截图。确认双击屏幕后串口打印了 "
              "===SHOT-BEGIN=== ... ===SHOT-END===。", file=sys.stderr)
        return 1

    out_dir = Path(args.dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")

    for index, (width, height, _expected, data) in enumerate(shots, start=1):
        if args.out and len(shots) == 1:
            bmp_path = Path(args.out)
        else:
            suffix = "" if len(shots) == 1 else f"-{index}"
            bmp_path = out_dir / f"shot-{stamp}{suffix}.bmp"
        bmp_path.write_bytes(data)
        print(f"{bmp_path}  ({width}x{height}, {len(data)} 字节)")
        if args.png:
            print(f"{to_png(bmp_path)}  (PNG)")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
