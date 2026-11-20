"""编译前生成固件版本号头文件（PlatformIO pre-script）。

用法：platformio.ini 中 extra_scripts = pre:scripts/bump_version.py

每次执行 pio run 时，把编译时刻的 年.月.日.时分（本地时间）写入
include/fw_version.h（如 2026.11.20.1419）。只在该文件内容变化时才写盘，
因此不触碰 config.h，不会引发大范围增量重编——只有包含 fw_version.h
的少数几个源文件（UsbOta.cpp / EventLog.cpp / LvglDashboard.cpp）会重编。
clean / erase 等非编译目标不触发。
"""

Import("env")

from datetime import datetime
from pathlib import Path

HEADER = Path(env["PROJECT_DIR"]) / "include" / "fw_version.h"
SKIP_TARGETS = {"clean", "erase", "erase_flash", "menuconfig", "uploadfs"}

targets = set(env.get("COMMAND_LINE_TARGETS", []))
if targets & SKIP_TARGETS:
    print("[version] skip for target(s): %s" % ", ".join(sorted(targets) or "(none)"))
else:
    version = datetime.now().strftime("%Y.%m.%d.%H%M")
    content = (
        "// 自动生成：scripts/bump_version.py（编译时按当前时间写入）。请勿手改。\n"
        "#pragma once\n"
        '#define CFG_FW_VERSION "%s"\n' % version
    )
    if HEADER.is_file() and HEADER.read_text(encoding="utf-8") == content:
        # 同一分钟内重复编译：不写盘，避免无谓的增量重编。
        print("[version] unchanged: %s" % version)
    else:
        with open(HEADER, "w", encoding="utf-8", newline="\n") as f:
            f.write(content)
        print("[version] %s" % version)
