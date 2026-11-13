# ESP32-S3 圆屏固件

面向微雪 Waveshare **ESP32-S3-Touch-AMOLED-1.75-B**（SKU 31262）的独立
PlatformIO 固件工程。本目录与现有 `TESLA-DASHBOARD` 分开，后续可复用仓库根目录的
`TeslaBleBackend` 作为车辆 BLE 数据源。

硬件参数依据微雪[产品页](https://www.waveshare.net/shop/ESP32-S3-Touch-AMOLED-1.75-B.htm)
及[官方硬件参考](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75/blob/main/HARDWARE_REFERENCE_ZH.md)。
`-B` 为带外壳版本；显示、触摸和主板连接与标准版相同。

| 项目 | 参数 |
| --- | --- |
| MCU | ESP32-S3R8 |
| Flash / PSRAM | 16 MB Flash / 8 MB OPI PSRAM |
| 圆形 AMOLED | 1.75 英寸，466 × 466，CO5300，QSPI |
| LCD 引脚 | SIO0–3: GPIO 4–7；PCLK: GPIO 38；CS: GPIO 12；RESET: GPIO 39 |
| 触摸 | CST9217 电容触摸，I²C 地址 `0x5A` |
| 触摸引脚 | SDA: GPIO 15；SCL: GPIO 14；INT: GPIO 11；RESET: GPIO 40 |
| 按键 | BOOT: GPIO0（按下为低）；PWR: 由 AXP2101 供电通路控制，经 TCA9554 EXIO4 读取 |

AMOLED 亮度由 CO5300 面板命令控制，没有独立背光 PWM 引脚；固件支持在屏幕上
上下滑动实时调节亮度（见“上下滑动调节亮度”一节）。
板载 I²C 是共享总线；不要另建占用 GPIO14/15 的独立 I²C 总线。

## 目录结构

```text
ESP32-S3-ROUND-DISPLAY/
├── platformio.ini
├── design/
│   ├── UI-DESIGN.md            # 圆屏界面设计说明（坐标/配色/状态/数据映射）
│   ├── dashboard-ui.html       # 1:1 可交互设计稿（浏览器打开）
│   └── preview-*.png           # 四个状态的预览图
├── include/
│   ├── config.h
│   ├── app/
│   │   └── DashboardApp.h
│   ├── core/
│   │   └── UiModel.h
│   ├── data/
│   │   └── IVehicleSource.h
│   ├── display/
│   │   ├── IDisplay.h
│   │   └── RoundDisplay.h
│   ├── input/
│   │   └── Cst9217Touch.h       # CST9217 触摸轮询驱动
│   ├── platform/
│   │   ├── BoardProfile.h
│   │   └── boards/
│   │       └── waveshare_esp32_s3_touch_amoled_175_b.h
│   └── ui/
│       ├── IDashboard.h
│       └── LvglDashboard.h
└── src/
    ├── app/
    │   └── DashboardApp.cpp
    ├── display/
    │   └── RoundDisplay.cpp
    ├── input/
    │   └── Cst9217Touch.cpp
    ├── ui/
    │   └── LvglDashboard.cpp
    └── main.cpp
```

## 界面设计

- `design/UI-DESIGN.md`：466×466 圆屏布局、安全区、配色/字号 token、状态机与数据映射
- `design/dashboard-ui.html`：浏览器打开的 1:1 设计稿（可勾选安全圆/中心线）
- `design/preview-*.png`：行驶、充电、断连扫描、配钥四个状态预览

设计稿沿用 `TESLA-DASHBOARD` 的黑底 / Tesla 红 / 绿色能量回收语言，但按圆屏重排：
速度环（R170，204°–336°）包围车速大字，顶部收窄状态带，底部只保留两张信息卡。
驾驶 / 充电共用同一套骨架（速度环 ↔ SOC 环），未连接和配钥使用整屏状态页。

## 界面实现

固件已通过 Arduino_GFX 初始化 CO5300 QSPI 面板，并用 LVGL 9 实现设计稿中的
行驶、充电、断连/扫描和配钥页面。实时数据由 `TeslaBleBackend` 提供：
`src/data/TeslaUiSource.cpp` 把 `VehicleState` / `SourceStatus` 映射成 `UiModel`，
`DashboardApp` 每轮主循环刷新一次。`include/config.h` 中的 `CFG_DEMO_MODE`
设为 `1` 可回到静态演示数据。CST9217 触摸已接入，用于上下滑动调节亮度（见下文）。

### 按键与画面旋转

板载 **PWR 键长按 3 秒** 会重启固件。PWR 不在 ESP32 的 GPIO 上，而是经
TCA9554（I2C `0x20`）的 EXIO4 读取（高电平 = 按下）：`DashboardApp` 启动时
把 EXIO4 配置为输入，主循环中 30 ms 消抖检测，按住满 3 秒调用
`ESP.restart()`。开机时若 PWR 仍被按住（例如正用它上电），会先等松开再进入
长按检测，避免上电动作被误判成重启。

短按板载 **BOOT 键（GPIO0，按下为低）** 可把画面顺时针旋转 90°：
0° → 90° → 180° → 270° → 0°，如此循环。实现分两层：

- `DashboardApp::pollBootButton()`：30 ms 消抖，检测到有效按下后调用
  `RoundDisplay::rotateClockwise()`；按住不放只触发一次。
- `RoundDisplay`：CO5300 的 MADCTL 只能做镜像、不能交换 XY，因此旋转在
  flush 回调里用软件完成——RGB565 像素块重排到一块 466×50 的临时缓冲
  （优先 PSRAM），再按旋转后的窗口坐标写给面板；切换时整屏标脏重绘。

旋转方向会保存到 NVS（namespace `display`，key `rot`）：每次按键后立即写入，
`RoundDisplay::begin()` 启动时读回，因此重启、断电或正常重新烧录固件后都会
保持上次的方向，开机第一帧即为该方向。只有全片擦除（`pio run -t erase` /
`esptool erase_flash`）、烧录包含 NVS 分区的整片镜像，或更换分区表时才会
恢复默认 0°；想手动恢复默认方向，按 BOOT 键转回 0° 覆盖保存即可。

板载只有 BOOT 和 PWR 两个按键，没有独立 RESET 键；复位由 `CHIP_PU` 控制，
程序无法把复位动作当成普通按键读取，所以旋转触发使用 BOOT 键，重启使用
PWR 长按。

### 上下滑动调节亮度

单指在屏幕上**上滑调亮、下滑调暗**，亮度实时跟手变化，松手后保存。触摸只用于
这个手势，**没有注册 LVGL indev**，因此不会与界面控件抢事件。

手势判定（坐标均为 LVGL 画面坐标，已按当前旋转换算）：

- 按住后纵向累计位移 `|dy| ≥ 24 px`，且 `|dy| > |dx| + 16 px` 才激活；
- 一旦判定为横向拖动，本次按下不再触发亮度手势；
- 激活后 `1 px ≈ 1 档`亮度，以激活点为锚点避免起始跳动；范围限制在
  **10%–80%**（26–204）之间，防止过亮或过暗。

三个阈值和灵敏度都在 `src/app/DashboardApp.cpp` 顶部：

```cpp
constexpr int32_t kSwipeActivatePx = 24;      // 激活所需纵向位移
constexpr int32_t kSwipeDominancePx = 16;     // 纵向需领先横向的幅度
constexpr int32_t kBrightnessStepPerPx = 1;   // 每像素对应亮度档位
```

**旋转感知**：触摸芯片报告的是面板物理坐标，而画面在 flush 时被软件旋转了
`m_rotation` 个 90°。`RoundDisplay::mapTouchToScreen()` 对坐标做
`blitRotated()` 的逆变换（0°/90°/180°/270° 分别对应不同的坐标映射），使
"屏幕上的上下"在任何旋转角度下都指向同一个手势方向，与用户看到的画面一致。

**亮度控制**：写 CO5300 的 0x51 寄存器（`Arduino_CO5300::setBrightness()`，
0–255）。`RoundDisplay::setBrightness()` 会把亮度统一钳制在
`kMinBrightness`–`kMaxBrightness`（26–204，即 10%–80%）之间，从 NVS 读回的
旧值也会重新钳制。拖动过程中只写寄存器；松手时才由
`RoundDisplay::saveBrightness()` 写回 NVS（namespace `display`，key `bri`），
避免频繁擦写 flash。默认亮度 160，重启/断电后保持上次调节结果。

**触摸驱动**（`src/input/Cst9217Touch.cpp`）：按 Waveshare 自带的 SensorLib
`TouchDrvCST92xx` 流程实现，复用 PWR 键那条 I2C 总线（GPIO15/14，从地址
`0x5A`）：

1. 拉低触摸 RESET（GPIO40）10 ms 复位，等待固件启动；
2. 写 `0xD1 0x01` 进命令模式，依次读固件校验码 `0xD1FC`（高 16 位应为
   `0xCACA`）、分辨率 `0xD1F8`、芯片类型 `0xD204`（`0x9217`）；
3. 主循环每 30 ms 轮询一次：写 `0xD000` 请求 15 字节报告帧，再写
   `0xD0 0x00 0xAB` 应答；只解析第一点的坐标（`event == 0x06` 表示按下）。

写寄存器地址与读数据之间发 **STOP**（`endTransmission()`）而不是 repeated
start——CST9217 对后者不应答。

调试时可看串口（115200）日志：

```text
[TOUCH] firmware info: .. .. CA CA
[TOUCH] CST9217 ready (res 466x466)
[TOUCH] press x=.. y=.. frame=......
[GESTURE] press rot=0 raw=(..,..) screen=(..,..)
[TOUCH] brightness swipe start (dx=.. dy=.. base 160)
[TOUCH] brightness saved -> 200
```

`[TOUCH] press` 每按下/松开各出现一次（空闲不刷屏）；`[GESTURE] press`
里的 `raw` 是面板原始坐标、`screen` 是旋转换算后的画面坐标，可用于核对触摸
方向是否与画面一致。

BLE 配置在 `include/config.h`：

```cpp
#define CFG_TESLA_VIN "LRWYGCFJ8RC571213"
#define CFG_TESLA_NAME ""        // 留空 = 由 VIN 推导广播名
#define CFG_TESLA_UNITS_MILES 1  // 真车速度/续航原始单位为英里
#define CFG_TESLA_CRYPTO 1       // 加密握手 + GetVehicleData
#define CFG_TESLA_DRIVE_POLL_INTERVAL_MS 100  // 驾驶状态轮询间隔
#define CFG_DEBUG_LOG 1          // 联调日志，稳定后可改 0
```

串口日志会输出前 10 次 Drive 请求从发送到收到响应的耗时（毫秒）及故障码；
请求仍保持串行，因此实际刷新间隔还受车端响应速度影响。

真实私钥放在 `include/secrets/tesla_client_key.h`（已被 `.gitignore` 忽略），
缺失时使用后端内置的测试密钥。工程通过 `lib_extra_dirs = ..` 引用仓库根目录的
`TeslaBleBackend`，无需复制库源码。

## PlatformIO

工程环境登记为 `waveshare_esp32_s3`，使用 ESP32-S3 Dev Module 定义，设置 16 MB Flash、
OPI PSRAM 与 USB CDC。依赖为 LVGL 9、Arduino_GFX（CO5300 QSPI）和 NimBLE-Arduino。微雪示例文档使用
LVGL 8.4；本工程使用 LVGL 9，并配置仅用于仪表盘的 Montserrat 字号。
