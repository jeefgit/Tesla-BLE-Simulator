"""编译前自动生成固件版本号（PlatformIO pre-script）。

用法：platformio.ini 中 extra_scripts = pre:scripts/bump_version.py

每次执行 pio run 时，把 include/config.h 里
    #define CFG_FW_VERSION "..."
改写为编译时刻的 年.月.日.时分（本地时间），如 2026.11.20.1419。
clean / erase 等非编译目标不触发。

注意：config.h 变更会带来本工程源文件的小规模增量重编，LVGL 等库不受影响。
"""

Import("env")

import re
from datetime import datetime
from pathlib import Path

CONFIG = Path(env["PROJECT_DIR"]) / "include" / "config.h"
VERSION_RE = re.compile(r'#define CFG_FW_VERSION "([^"]*)"')
SKIP_TARGETS = {"clean", "erase", "erase_flash", "menuconfig", "uploadfs"}

targets = set(env.get("COMMAND_LINE_TARGETS", []))
if targets & SKIP_TARGETS:
    print("[version] skip bump for target(s): %s" % ", ".join(sorted(targets) or "(none)"))
else:
    if not CONFIG.is_file():
        print("[version] ERROR: not found: %s" % CONFIG)
    else:
        new_version = datetime.now().strftime("%Y.%m.%d.%H%M")
        # newline="" 保持原文件的 \r\n / \n 行尾不变，避免整文件 diff。
        with open(CONFIG, "r", encoding="utf-8", newline="") as f:
            text = f.read()
        m = VERSION_RE.search(text)
        if not m:
            print("[version] ERROR: CFG_FW_VERSION not found in config.h")
        else:
            old_line = m.group(0)
            new_line = '#define CFG_FW_VERSION "%s"' % new_version
            if old_line == new_line:
                # 同一分钟内重复编译：不回写，避免无谓的增量重编。
                print("[version] unchanged: %s" % new_version)
            else:
                text = text.replace(old_line, new_line, 1)
                with open(CONFIG, "w", encoding="utf-8", newline="") as f:
                    f.write(text)
                print("[version] %s -> %s" % (m.group(1), new_version))
