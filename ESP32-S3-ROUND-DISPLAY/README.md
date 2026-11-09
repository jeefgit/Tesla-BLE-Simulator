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
├── include/
│   ├── config.h
│   ├── app/
│   │   └── DashboardApp.h
│   ├── core/
│   │   └── UiModel.h
│   ├── data/
│   │   └── IVehicleSource.h
│   ├── display/
│   │   └── IDisplay.h
│   ├── platform/
│   │   ├── BoardProfile.h
│   │   └── boards/
│   │       └── waveshare_esp32_s3_touch_amoled_175_b.h
│   └── ui/
│       └── IDashboard.h
└── src/
    └── main.cpp
```

## 后续开发

1. 实现 CO5300 QSPI 显示初始化与 LVGL flush。
2. 接入 CST9217 触摸输入及圆形屏的触控坐标映射。
3. 实现 `display/` 与 `ui/`，再连接 `TeslaBleBackend` 数据源。
4. 接实板验证 PlatformIO Flash、PSRAM 与分区设置。

## PlatformIO

工程环境登记为 `waveshare_esp32_s3`，使用 ESP32-S3 Dev Module 定义，设置 16 MB Flash、
OPI PSRAM 与 USB CDC。依赖为 LVGL 9、Arduino_GFX（CO5300 QSPI）和 NimBLE-Arduino。
微雪示例文档使用 LVGL 8.4；本工程保留现有项目的 LVGL 9 版本，接入显示驱动时需验证
LVGL 9 集成接口。本次仅补齐板级参数与工程配置，尚未实现驱动、触摸或构建验证。
