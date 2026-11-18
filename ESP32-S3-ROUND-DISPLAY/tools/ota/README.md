# USB OTA 网页升级工具

不依赖 WiFi：ESP32-S3 通过板载 USB 口连到电脑，浏览器用 **Web Serial** 把
`firmware.bin` 发进设备，固件写入另一个 OTA app 分区，CRC32 校验通过后自动
重启。旋转、亮度等 NVS 设置会保留。

## 使用步骤

1. 编译（用户自行执行）：

   ```bash
   cd ESP32-S3-ROUND-DISPLAY
   pio run
   ```

   产物：`.pio/build/waveshare_esp32_s3/firmware.bin`（**只要 app 镜像**，
   不要用 `firmware.merged.bin`、bootloader 或 partitions 镜像）。

2. 用 USB 线把设备接到电脑。若设备已经自动休眠，先触摸屏幕或摇晃设备唤醒。

3. 双击 `run.bat`（或执行 `py serve.py` / `python3 serve.py`），浏览器会打开
   <http://localhost:8787/index.html>。

   > 必须用 Chrome / Edge 89+。Web Serial 只在 `https` 或 `localhost`
   > 下可用，直接双击 `index.html`（`file://`）不行。
   >
   > 串口监视器（`pio device monitor`、串口助手）会独占串口，连接前先关掉。

4. 点“连接设备”，在浏览器弹窗里选择 ESP32 的串口，等待显示
   `已连接（固件 x.y.z）`。

5. 选择/拖入 `firmware.bin`，点“开始升级”。屏幕会显示 `USB OTA` 进度页，
   完成后设备自动重启。

6. 重连一次，确认设备固件版本已更新。

## 协议（`src/ota/UsbOta.cpp`）

串口为 USB CDC，文本命令 + 二进制数据块，`\n` 结尾：

| 方向 | 内容 | 说明 |
| --- | --- | --- |
| 主机 → 设备 | `OTA_PING\n` | 握手 / 心跳 |
| 设备 → 主机 | `OTA: PONG <version>\n` | `CFG_FW_VERSION` |
| 主机 → 设备 | `OTA_BEGIN <size> <crc32-hex>\n` | app 镜像字节数 + CRC32 |
| 设备 → 主机 | `OTA: READY <size>\n` / `OTA: ERR begin ...\n` | 已调用 `Update.begin()` |
| 主机 → 设备 | ≤4096 字节二进制块 | 每块最多 `CFG_USB_OTA_CHUNK` |
| 设备 → 主机 | `OTA: A <received>\n` | 每块 ACK，含累计字节数 |
| 设备 → 主机 | `OTA: OK\n` / `OTA: ERR crc|end|timeout|write ...\n` | 最终结果 |

设计要点：

- **逐块 ACK**：设备收到一块、写 flash、再回 ACK；主机收到 ACK 才发下一块。
  flash 写入期间没有数据在途，避免 USB CDC 接收缓冲溢出。
- **CRC32 先校验再切换分区**：CRC 不匹配时 `Update.abort()`，不会切换启动分区。
- **超时**：单块 10 秒（`CFG_USB_OTA_RX_TIMEOUT_MS`）。中途关闭页面/拔线时，
  设备会在超时后自动放弃并回到仪表，不需要重启。
- **心跳**：页面每 5 秒发一次 `OTA_PING`，设备会重置自动休眠计时。

## 限制与回滚

- 写入的是 `default_16MB.csv` 里的 `app0` / `app1`（各 6.25MB），分区表不用改。
- 没有启用 bootloader 自动回滚。如果新固件启动即崩溃，需要再用 USB
  （`pio run -t upload` 或 esptool）重新烧录。
- OTA 成功后会从另一个 app 分区启动。`pio run -t upload` 会同时写入
  `boot_app0.bin`（重置 otadata 选择 app0），所以不受影响；如果用 esptool
  只手动写 `0x10000` 的 `firmware.bin`，otadata 可能仍指向 OTA 分区，
  需要一并写 `boot_app0.bin` 或擦除 `otadata`。
- 升级期间 BLE 轮询会暂停，设备重启后自动重新扫描连接车辆。
- 若要把 OTA 关掉：`include/config.h` 里 `CFG_USB_OTA_ENABLE` 改 0。
- 每次发版建议同步改 `CFG_FW_VERSION`，页面握手时会显示，便于确认升级结果。
