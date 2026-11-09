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

AMOLED 亮度由 CO5300 面板命令控制，没有独立背光 PWM 引脚。
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
行驶、充电、断连/扫描和配钥页面。当前 `DashboardApp` 使用演示快照展示行驶页面；
要显示实时车辆数据，仍需将 `TeslaBleBackend` 接入 `UiModel`。CST9217 触摸输入尚未接入。

## PlatformIO

工程环境登记为 `waveshare_esp32_s3`，使用 ESP32-S3 Dev Module 定义，设置 16 MB Flash、
OPI PSRAM 与 USB CDC。依赖为 LVGL 9、Arduino_GFX（CO5300 QSPI）和 NimBLE-Arduino。微雪示例文档使用
LVGL 8.4；本工程使用 LVGL 9，并配置仅用于仪表盘的 Montserrat 字号。
