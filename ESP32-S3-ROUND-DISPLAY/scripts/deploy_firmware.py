"""编译后自动发布固件到上层 firmware 目录（PlatformIO post-script）。

用法：platformio.ini 中 extra_scripts = ... , post:scripts/deploy_firmware.py

firmware.bin 编译完成后，复制到
    <仓库根>/firmware/ESP32-S3-ROUND-DISPLAY/v<版本号>/firmware.bin
并生成同目录 SHA256SUMS.txt（大写哈希，格式与既有发布目录一致）。

版本号从 include/config.h 的 CFG_FW_VERSION 读取（由 bump_version.py 在
编译前写入，因此 bin 里的版本与目录名一定一致）。clean / erase 等目标不触发。
"""

Import("env")

import hashlib
import re
import shutil
from pathlib import Path

CONFIG = Path(env["PROJECT_DIR"]) / "include" / "config.h"
SKIP_TARGETS = {"clean", "erase", "erase_flash", "menuconfig", "uploadfs"}


def deploy(source, target, env):
    with open(CONFIG, "r", encoding="utf-8", newline="") as f:
        m = re.search(r'#define CFG_FW_VERSION "([^"]*)"', f.read())
    if not m:
        print("[deploy] ERROR: CFG_FW_VERSION not found in config.h")
        return

    version = m.group(1)
    bin_path = Path(str(target[0]))
    if not bin_path.is_file():
        print("[deploy] ERROR: firmware not found: %s" % bin_path)
        return

    dest_dir = Path(env["PROJECT_DIR"]).parent / "firmware" / "ESP32-S3-ROUND-DISPLAY" / ("v" + version)
    dest_dir.mkdir(parents=True, exist_ok=True)

    dest_bin = dest_dir / bin_path.name
    shutil.copyfile(bin_path, dest_bin)

    sha = hashlib.sha256(dest_bin.read_bytes()).hexdigest().upper()
    with open(dest_dir / "SHA256SUMS.txt", "w", encoding="utf-8", newline="\n") as f:
        f.write("%s  %s" % (sha, bin_path.name))

    print("[deploy] %s -> %s" % (version, dest_dir))
    print("[deploy] sha256: %s" % sha)


targets = set(env.get("COMMAND_LINE_TARGETS", []))
if targets & SKIP_TARGETS:
    print("[deploy] skip for target(s): %s" % ", ".join(sorted(targets) or "(none)"))
else:
    # 挂在 firmware.bin 生成之后：只有真正编译出新 bin 才发布。
    env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", deploy)
