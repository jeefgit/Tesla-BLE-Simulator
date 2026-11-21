# Firmware v2026.11.21.2332

- 版本：`2026.11.21.2332`（`ESP32-S3-ROUND-DISPLAY/include/fw_version.h` 的 `CFG_FW_VERSION`）
- 构建环境：PlatformIO `waveshare_esp32_s3`
- 文件：`firmware.bin`（仅 app 镜像，用于 USB OTA）
- 大小：2094592 字节
- 编译时间：2026-11-21 23:34
- SHA256：`8A7BF870D82398197AD91B76BE115018C5C39185F6D4A07A0BC4EF9CBA88B1DE`

## 今日变更（2026-11-21）

### 1. 配置模式独立页面（53dd994 优化配置）

- 新增 `LvglDashboard` 配置模式页 `buildConfigPage()`：显示 `CONFIG MODE` 标题、
  热点 SSID、密码、网页地址 `http://<IP>` 和操作提示，替代原先停留在扫描链接页的画面。
- `LvglDashboard::setConfigMode()` / `setConfigInfo()`：配置页按需构建（仅进入配置模式时
  创建），避免十几个控件常驻占用系统堆；退出配置模式后页面隐藏。
- `WebConfigServer` 新增 `ssid()` / `ip()` 接口，`begin()` 时记录实际热点名与 AP IP，
  供仪表盘显示。
- `DashboardApp::begin()` 在首帧渲染前就启动热点并刷新配置页信息，解决配置页一直显示
  扫描动画的问题。
- 绘制缓冲分配顺序反转（`RoundDisplay.cpp`）：优先 PSRAM、内部 SRAM 兜底。466x50x2
  ≈ 46.6KB，放内部 SRAM 会挤占 NimBLE 连接任务所需的 8KB 连续内存，导致 BLE 任务创建失败；
  绘制缓冲放 PSRAM 不影响刷屏（QSPI 驱动会先拷入 DMA 缓冲）。
- `CFG_TESLA_DRIVE_POLL_INTERVAL_MS` 由 155 调整为 500（驾驶状态轮询间隔）。

### 2. 车速显示过渡优化（903d7d8 优化速度 显示）

- 车速数字与弧线过渡时长 `kSpeedTransitionMs` 由 30ms 调整为 400ms，约跨 8 个重绘帧
  （重绘节流 50ms），消除 20 → 17 的瞬间跳变。
- 新增 `m_speedWasValid`：车速无效或刚从无效恢复有效时直接贴目标值，避免从 0 逐渐扫上去；
  有效状态下连续更新则从当前显示值平滑过渡，防止回跳。

## 升级方式

使用 `ESP32-S3-ROUND-DISPLAY/tools/ota/` 的 USB OTA 页面，选择本目录的
`firmware.bin` 发送到设备即可。
