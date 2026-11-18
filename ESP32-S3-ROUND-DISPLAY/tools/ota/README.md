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

4. 点“连接设备”。**首次**需要在浏览器弹窗里选择一次 ESP32 串口；之后若
   只授权过这一个设备，页面会直接连接、不再弹框。要换设备时按住 `Shift`
   点“连接设备”强制重新选择。等待显示 `已连接（固件 x.y.z）`。

   > Web Serial 不允许页面枚举系统串口，所以“只有一台设备”只能通过已授权的
   > 端口数量判断；全新浏览器/全新设备第一次必然要选一次。

5. 选择/拖入 `firmware.bin`，点“开始升级”。屏幕会显示 `USB OTA` 进度页，
   完成后设备自动重启。

6. 升级成功后页面会自动断开，**等 10 秒**再自动重连（最多重试 3 次），并在
   日志里给出“升级是否生效”的结论。想立刻连接可以随时点“连接设备”打断倒计时。

## 协议（`src/ota/UsbOta.cpp`）

串口为 USB CDC，文本命令 + 二进制数据块，`\n` 结尾：

| 方向 | 内容 | 说明 |
| --- | --- | --- |
| 主机 → 设备 | `OTA_PING\n` | 握手 / 心跳 |
| 设备 → 主机 | `OTA: PONG ver=<v> build=<日期 时间> part=<app0|app1>\n` | 版本 + 编译时间 + 当前运行分区 |
| 主机 → 设备 | `OTA_INFO\n` | 体检：两个分区各是什么镜像 |
| 设备 → 主机 | `OTA: INFO run=<p> boot=<p> state=<n>\n` | 当前运行 / 启动分区 / OTA 状态 |
| 设备 → 主机 | `OTA: IMG app0=<v>@<日期 时间>\n`、`OTA: IMG app1=...` | 各分区镜像；空分区为 `empty` |
| 主机 → 设备 | `OTA_REBOOT\n` | 单独测试重启是否可用 |
| 主机 → 设备 | `OTA_BEGIN <size> <crc32-hex>\n` | app 镜像字节数 + CRC32 |
| 设备 → 主机 | `OTA: INFO incoming=<v> built <日期 时间>\n` | 收到第一块时读出的“你要刷的镜像” |
| 设备 → 主机 | `OTA: INFO current=...` / `OTA: INFO same_image\n` | 与当前运行版本对比；完全相同时告警 |
| 设备 → 主机 | `OTA: READY <size>\n` / `OTA: ERR begin ...\n` | 已调用 `Update.begin()` |
| 主机 → 设备 | ≤4096 字节二进制块 | 每块最多 `CFG_USB_OTA_CHUNK` |
| 设备 → 主机 | `OTA: A <received>\n` | 每块 ACK，含累计字节数 |
| 设备 → 主机 | `OTA: INFO readback=ok\n` | 回读 Flash 重算 CRC 与主机一致 |
| 设备 → 主机 | `OTA: INFO written=<v> built <日期 时间> part=<label>\n` | 从刚写入的镜像头读出的信息 |
| 设备 → 主机 | `OTA: OK boot=<label>\n` / `OTA: ERR crc|end|timeout|write|readback ...\n` | 最终结果 |

**排查“OTA 成功但没变化”**

页面右上角会显示 `页面版本：ota-page-5`（不是就 Ctrl+Shift+R 强刷）。连上设备
后会自动执行一次 `OTA_INFO`，日志里会出现三行：

```
OTA: INFO run=app0 boot=app1 state=1
OTA: IMG app0=0.1.2@Nov 18 2026 16:06:36
OTA: IMG app1=0.1.2@Nov 18 2026 16:20:01
```

- `run=app0 boot=app1` → otadata 已经指向 app1，但设备还在跑 app0，说明
  **重启后没换过去**（回滚或根本没重启）；点“重启设备”按钮可以单独验证重启。
- `app0` 和 `app1` 的构建时间相同 → 两个分区里是同一版固件，看不出变化很正常。
- 开始升级时设备还会打印 `OTA: INFO incoming=...` 和 `current=...`，直接对比
  “你传的”和“正在跑的”是不是同一个构建；相同会额外打印 `same_image`。

**怎么确认升级真的生效**

`OTA: PONG` 里的 `build=` 是编译器自动填的编译时间戳（`__DATE__ __TIME__`），
`part=` 是当前运行的分区。升级前记下这两个值，升级后重连再看一次：

- `build` 变了 → 设备确实跑上了新固件；
- `build` 没变 → 传进去的就是同一个镜像（多半是忘了重新编译，或选错了文件）；
- `part` 从 `app0` 变 `app1`（或反过来）→ 说明启动分区确实切换了。

网页会自动做这个对比并在日志里给出结论。

设计要点：

- **逐块 ACK**：设备收到一块、写 flash、再回 ACK；主机收到 ACK 才发下一块。
  flash 写入期间没有数据在途，避免 USB CDC 接收缓冲溢出。
- **CRC32 先校验再切换分区**：CRC 不匹配时 `Update.abort()`，不会切换启动分区。
- **超时**：单块 10 秒（`CFG_USB_OTA_RX_TIMEOUT_MS`）。中途关闭页面/拔线时，
  设备会在超时后自动放弃并回到仪表，不需要重启。
- **心跳**：页面每 5 秒发一次 `OTA_PING`，设备会重置自动休眠计时。

## 限制与回滚

- 写入的是 `default_16MB.csv` 里的 `app0` / `app1`（各 6.25MB），分区表不用改。
- 这个 core 的 sdkconfig 打开了 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`，所以
  新镜像首次启动处于 `PENDING_VERIFY`；Arduino 的 `initArduino()` 会在执行
  `setup()` 之前调用 `verifyOta()`（默认返回 true）并立即
  `esp_ota_mark_app_valid_cancel_rollback()`。也就是说：只要新固件能跑到
  `setup()`，就不会被回滚；如果它连早于 `initArduino()` 的阶段都起不来，
  bootloader 会自动回滚到旧固件——那种情况下的表现就是“OTA 成功但版本没变”。
  串口日志里的 `OTA: INFO boot_state=` 会显示分区状态。
- OTA 成功后会从另一个 app 分区启动。`pio run -t upload` 会同时写入
  `boot_app0.bin`（重置 otadata 选择 app0），所以不受影响；如果用 esptool
  只手动写 `0x10000` 的 `firmware.bin`，otadata 可能仍指向 OTA 分区，
  需要一并写 `boot_app0.bin` 或擦除 `otadata`。
- 升级期间 BLE 轮询会暂停，设备重启后自动重新扫描连接车辆。
- 若要把 OTA 关掉：`include/config.h` 里 `CFG_USB_OTA_ENABLE` 改 0。
- 每次发版建议同步改 `CFG_FW_VERSION`；但判断是否真的升级成功请看 `build=`
  时间戳，而不是版本号。
