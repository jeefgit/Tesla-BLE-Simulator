#!/usr/bin/env python3
"""仪表盘 UI 预览渲染器（320x240，1:1 像素对齐 TESLA-DASHBOARD 的 LVGL 实现）。

用途：改布局前先在 PC 上出图，避免反复烧板；输出可直接贴进 issue / README。
不依赖工程代码，只按 dashboard/DESIGN.md 的坐标重画一遍，所以是「近似稿」：
字体用系统 Arial 代替 LVGL Montserrat，圆角和字宽会有 1–2px 差异。

用法：
    pip install pillow
    python dashboard/preview/mock_render.py

产物（与本脚本同目录）：
    dashboard_states.png     4 状态横排（正常 / 车门+胎压+低电 / 充电 / 断连）
    dashboard_normal_2x.png  正常状态 2x 放大（看细节）

配色与 DESIGN.md 第 4 节一致；坐标与 Dashboard.cpp 同步，改 UI 时两边一起改。
"""

import os
import sys

from PIL import Image, ImageDraw, ImageFont

W, H = 320, 240
OUT_DIR = os.path.dirname(os.path.abspath(__file__))

# ---------------------------------------------------------------- 字体
FONT_CANDIDATES = [
    (r"C:\Windows\Fonts\arialbd.ttf", r"C:\Windows\Fonts\arial.ttf"),
    ("/System/Library/Fonts/Supplemental/Arial Bold.ttf",
     "/System/Library/Fonts/Supplemental/Arial.ttf"),
    ("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
     "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"),
]
FONT_BOLD = FONT_REG = None
for bold_path, reg_path in FONT_CANDIDATES:
    if os.path.exists(bold_path) and os.path.exists(reg_path):
        FONT_BOLD, FONT_REG = bold_path, reg_path
        break
if FONT_BOLD is None:
    print("warning: 没找到 Arial/DejaVu 字体，退回 Pillow 内置位图字体（预览会比较粗糙）")


def F(size, bold=True):
    path = FONT_BOLD if bold else FONT_REG
    if path:
        return ImageFont.truetype(path, size)
    return ImageFont.load_default()


# ---------------------------------------------------------------- 色板（DESIGN.md §4.1）
C_BG = (0, 0, 0)
C_FG = (255, 255, 255)
C_DIM = (138, 147, 166)
C_FAINT = (90, 99, 119)
C_LINE = (20, 24, 31)
C_TRACK = (14, 18, 25)
C_BRD = (30, 36, 48)
C_GEAR_OFF = (51, 58, 71)
C_REGEN = (33, 193, 94)
C_ALERT = (255, 69, 58)
C_CAUTION = (255, 179, 64)
C_TPMS_OK = (74, 81, 99)
C_OFFLINE = (35, 42, 54)
C_CAR_LINE = (57, 65, 79)
C_DOOR_OFF = (20, 24, 31)

# ---------------------------------------------------------------- 几何（与 Dashboard.cpp 同步）
CAR_X, CAR_Y, CAR_W, CAR_H = 26, 48, 52, 120
TPMS_BLK = [(0, CAR_Y + 26), (CAR_W + 32, CAR_Y + 26),
            (0, CAR_Y + 70), (CAR_W + 32, CAR_Y + 70)]
TPMS_BAR_W = 16
BANNER_Y = H - 16


def txt(d, x, y, w, s, size, color, align="c", bold=True, ls=0):
    """在 (x, y, w) 区域内按对齐方式画文字；ls = 额外字距。"""
    f = F(size, bold)
    if ls:
        widths = [d.textlength(ch, font=f) for ch in s]
        total = sum(widths) + ls * max(0, len(s) - 1)
        cx = x + (w - total) / 2 if align == "c" else (x if align == "l" else x + w - total)
        for ch, cw in zip(s, widths):
            d.text((cx, y), ch, font=f, fill=color)
            cx += cw + ls
    else:
        tw = d.textlength(s, font=f)
        cx = x + (w - tw) / 2 if align == "c" else (x if align == "l" else x + w - tw)
        d.text((cx, y), s, font=f, fill=color)


def rr(d, x, y, w, h, r, fill, outline=None):
    if w <= 0 or h <= 0:
        return
    d.rounded_rectangle([x, y, x + w - 1, y + h - 1], radius=r,
                        fill=fill, outline=outline)


def screen(drive=True, gear="D", t_out="23.5°", t_in="25.0°", locked=True,
           tpms=("2.9", "3.0", "2.8", "3.1"), tpms_bad=(), doors=(),
           speed="128", soc=78, rng="312", charging=False, kw=110,
           banner=None, link="LINK", ble_col=C_REGEN):
    im = Image.new("RGB", (W, H), C_BG)
    d = ImageDraw.Draw(im)

    # ---------------- 顶栏 y0-24
    for i, g in enumerate("PRND"):
        on = drive and g == gear
        txt(d, 9 + i * 18, 3, 16, g, 14, C_FG if on else C_GEAR_OFF)

    txt(d, 88, 9, 20, "OUT", 8, C_FAINT, "l", ls=1)
    txt(d, 108, 4, 36, t_out if drive else "--.-", 14, C_FG, "l")
    d.rectangle([146, 7, 146, 17], fill=C_BRD)
    txt(d, 152, 9, 14, "IN", 8, C_FAINT, "l", ls=1)
    txt(d, 168, 4, 36, t_in if drive else "--.-", 14, C_FG, "l")

    txt(d, 240, 4, 34, "LOCK" if (locked and drive) else "UNLK", 14,
        C_DIM if (locked and drive) else C_CAUTION, "r")
    txt(d, 278, 6, 38, link, 10, ble_col, "r")
    d.rectangle([0, 23, W, 23], fill=C_LINE)

    # ---------------- 车身栏 x0-104
    if not charging:
        txt(d, 0, 30, 104, "TIRE PRESSURE", 8, C_FAINT, ls=1)

        by = 24 + CAR_Y
        d.rounded_rectangle([CAR_X, by, CAR_X + CAR_W - 1, by + CAR_H - 1],
                            radius=14, fill=C_TRACK, outline=C_CAR_LINE)
        # 车顶中线 + 前后风挡
        d.rectangle([CAR_X + CAR_W // 2, by + 8, CAR_X + CAR_W // 2,
                     by + CAR_H - 8], fill=C_LINE)
        rr(d, CAR_X + 5, by + 16, CAR_W - 10, 10, 4, (23, 28, 38))
        rr(d, CAR_X + 5, by + CAR_H - 26, CAR_W - 10, 10, 4, (23, 28, 38))

        # 6 开口（关闭 = 暗色区块，打开 = 红色）
        door_boxes = [(2, 22, 5, 30), (CAR_W - 7, 22, 5, 30),
                      (2, 60, 5, 30), (CAR_W - 7, 60, 5, 30),
                      (12, 2, CAR_W - 24, 6), (12, CAR_H - 8, CAR_W - 24, 6)]
        for i, b in enumerate(door_boxes):
            rr(d, CAR_X + b[0], by + b[1], b[2], b[3], 2,
               C_ALERT if i in doors else C_DOOR_OFF)

        # 4 车轮
        wheels = [(CAR_X - 6, CAR_Y + 26), (CAR_X + CAR_W, CAR_Y + 26),
                  (CAR_X - 6, CAR_Y + 70), (CAR_X + CAR_W, CAR_Y + 70)]
        for i, (wx, wy) in enumerate(wheels):
            col = C_ALERT if (drive and i in tpms_bad) else (
                C_TPMS_OK if drive else C_OFFLINE)
            rr(d, wx, 24 + wy, 6, 24, 2, col)

        # 4 组胎压块：轮位 8px + 数值 10px + 16x3 迷你量程条
        for i, (bx, by2) in enumerate(TPMS_BLK):
            bad = i in tpms_bad
            txt(d, bx, 24 + by2, 20, "FL FR RL RR".split()[i], 8,
                C_ALERT if bad else C_FAINT, ls=1)
            txt(d, bx, 24 + by2 + 9, 20, tpms[i] if drive else "--", 10,
                C_ALERT if bad else (C_FG if drive else C_OFFLINE))
            rr(d, bx + 2, 24 + by2 + 23, TPMS_BAR_W, 3, 2, C_TRACK, C_BRD)
            if drive:
                try:
                    bar = float(tpms[i])
                except ValueError:
                    bar = 0.0
                frac = max(0.0, min(1.0, (bar - 1.5) / 2.0))
                rr(d, bx + 2, 24 + by2 + 23, int(frac * TPMS_BAR_W), 3, 2,
                   C_ALERT if bad else C_TPMS_OK)

    # ---------------- 中区 x104-250（充电时扩到 320）
    if not charging:
        txt(d, 104, 24 + 70, 146, speed if drive else "--", 48,
            C_FG if drive else C_OFFLINE, bold=False)
        txt(d, 104, 24 + 130, 146, "KM/H", 12,
            C_DIM if drive else C_OFFLINE, ls=3)
    else:
        # ⚡ 闪电
        d.polygon([(104 + 24, 24 + 30), (104 + 8, 24 + 52), (104 + 20, 24 + 52),
                   (104 + 14, 24 + 72), (104 + 34, 24 + 44), (104 + 22, 24 + 44)],
                  fill=C_REGEN)
        txt(d, 104 + 50, 24 + 24, 146, f"{kw:.1f} kW", 26, C_REGEN, "l")
        # 功率柱 0-250kW
        rr(d, 104 + 196, 24 + 20, 10, 110, 5, C_TRACK, C_BRD)
        fh = int(max(0.0, min(1.0, kw / 250.0)) * 110)
        rr(d, 104 + 196, 24 + 20 + 110 - fh, 10, fh, 5, C_REGEN)
        # SOC 进度条 + ETA
        rr(d, 104 + 14, 24 + 150, 188, 10, 5, C_TRACK, C_BRD)
        rr(d, 104 + 14, 24 + 150, int(soc * 188 / 100), 10, 5, C_REGEN)
        txt(d, 104 + 14, 24 + 172, 188, "EST REMAINING 1H 12M", 14, C_DIM)

    # ---------------- 电量列 x250-320
    if not charging:
        d.rectangle([250, 24, 250, 239], fill=C_LINE)
        low = drive and soc <= 15
        txt(d, 250, 24 + 8, 54, str(soc) if drive else "--", 26,
            C_ALERT if low else C_FG, "r")
        txt(d, 250 + 55, 24 + 22, 15, "%", 12, C_ALERT if low else C_DIM, "l")

        rr(d, 250 + 28, 24 + 46, 14, 110, 7, C_TRACK, C_BRD)
        if drive:
            fh = int(soc * 110 / 100)
            rr(d, 250 + 28, 24 + 46 + 110 - fh, 14, fh, 7,
               C_ALERT if low else C_FG)

        txt(d, 250, 24 + 164, 70, f"{rng} km" if drive else "-- km", 16, C_FG)
        txt(d, 250, 24 + 190, 70, "RANGE", 8, C_FAINT, ls=1)

    # ---------------- 底部告警 ticker y224-240
    if banner:
        col = C_ALERT if banner.startswith("RL TIRE") or "DOOR" in banner else C_CAUTION
        d.rectangle([0, BANNER_Y, W, H - 1], fill=(52, 16, 14) if col == C_ALERT
                    else (48, 34, 10))
        txt(d, 0, BANNER_Y - 1, W, "\u26a0 " + banner, 13, col)

    return im


def offline_screen():
    im = screen(drive=False, link="SCAN", ble_col=C_CAUTION)
    d = ImageDraw.Draw(im)
    d.rectangle([0, 24, W, H], fill=(0, 0, 0))
    txt(d, 0, 24 + 92, W, "Waiting for Tesla BLE...", 16, C_DIM)
    for i in range(3):
        d.ellipse([W // 2 - 20 + i * 14, 24 + 124, W // 2 - 15 + i * 14,
                   24 + 129], fill=(60, 70, 90))
    return im


def main():
    states = [
        ("DRIVE", screen()),
        ("ALERT  door + tpms + low soc",
         screen(tpms=("2.9", "3.0", "1.9", "3.1"), tpms_bad=(2,), doors=(0,),
                banner="RL TIRE 1.90 bar", speed="64", soc=12, rng="48",
                locked=False)),
        ("CHARGING", screen(charging=True, soc=62, kw=110, gear="P")),
        ("OFFLINE / SCANNING", offline_screen()),
    ]

    pad, gap, cap_h = 12, 10, 22
    strip = Image.new("RGB", (pad * 2 + W * len(states) + gap * (len(states) - 1),
                              pad * 2 + H + cap_h), (40, 40, 40))
    d = ImageDraw.Draw(strip)
    for i, (name, img) in enumerate(states):
        x = pad + i * (W + gap)
        strip.paste(img, (x, pad))
        txt(d, x, pad + H + 4, W, name, 12, (200, 206, 218), "c", bold=False)
    strip.save(os.path.join(OUT_DIR, "dashboard_states.png"))

    normal = screen().resize((W * 2, H * 2), Image.NEAREST)
    normal.save(os.path.join(OUT_DIR, "dashboard_normal_2x.png"))

    print("ok ->", OUT_DIR)


if __name__ == "__main__":
    sys.exit(main())
