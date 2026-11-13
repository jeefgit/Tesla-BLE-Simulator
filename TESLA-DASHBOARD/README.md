# ESP32 BLE Tesla Dashboard

基于 ESP32 的车载仪表盘：ESP32 作为 **BLE 中心设备（Central/Client）** 扫描并连接
**特斯拉 BLE**（模拟器 `sim_server.py` 或真车），解析加密车辆数据或 53 字节
`SimPayload` 遥测帧，再由仪表层渲染。

> **数据源为特斯拉 BLE**，支持真车加密数据和模拟器旁路数据。

## 技术选型

| 项 | 选择 | 说明 |
| --- | --- | --- |
| 框架 | PlatformIO + Arduino | 生态成熟，库管理方便 |
| 蓝牙库 | NimBLE-Arduino | 比 Bluedroid 省 200KB+ 内存 |
| BLE 角色 | Client（连车端） | 扫描（精确广播名） → 连接 → 订阅通知 |
| 协议 | 加密 GetVehicleData / 53B 定长二进制帧 | 支持真车加密轮询与模拟器旁路数据 |
| 显示 | 仪表应用使用 LVGL 9 + LovyanGFX | 后端库不依赖 UI 或显示驱动 |

## 架构

```
特斯拉模拟器 / 真车
     │  BLE Notify（加密 GetVehicleData 或 53B SimPayload）
     ▼
┌────────────┐  字节   ┌──────────────┐         ┌─────────────────┐
│  BleLink   │ ──────► │ TeslaClient  │ ──────► │ TeslaBleSource  │
│ 扫描/连接/  │  队列   │ 组帧/解密/配钥 │         │ 实现 IVehicleSource│
│ 订阅通知    │         │              │         └────────┬────────┘
└────────────┘         └──────────────┘                  │
                                                          ▼
                                              ┌─────────────────────┐
                                              │ AppController       │
                                              │ 组装 UiModel(只读快照)│
                                              └──────────┬──────────┘
                                                         │
                              ┌──────────────────────────┴───────────┐
                              ▼                                      ▼
                    ┌──────────────────┐                   ┌──────────────────┐
                    │ Dashboard (UI)   │                   │ Display (平台)    │
                    │ 只依赖 core/     │                   │ BoardProfile 驱动 │
                    └──────────────────┘                   └──────────────────┘
```

- **BleLink**（链路层）：扫描 → 连接 → 订阅，收到的字节只入环形缓冲，loop 里再交给上层。
- **TeslaClient**（协议层）：组帧/解密/配钥，写入 `g_vehicle`；超时后仪表降级显示 `--`。
- **TeslaBleSource / MockSource**（数据源层）：实现 `IVehicleSource`，把内部状态映射成
  `LinkState` + `SourceStatus`，UI 不感知 BLE 细节。
- **AppController**（装配层）：`source.loop()` → 组装 `UiModel` → `ui.render()` → `display.loop()`。
- **Dashboard**（UI 层）：只消费 `core/UiModel.h`，换成别的屏/布局时新增 `IDashboard` 实现即可。
- **Display / BoardProfile**（平台层）：引脚、面板、旋转、缓冲全部来自板型表，换板不改业务代码。
- **TeslaBleBackend**（依赖库）：位于仓库根目录，与 `TESLA-DASHBOARD/` 并列；封装 NimBLE 链路、Tesla 协议和车辆状态接口，不依赖仪表 UI 或显示驱动。

详见 `docs/ARCHITECTURE.md`。

## 协议要点

| 项 | 值 |
| --- | --- |
| 广播名 | `S + sha1(VIN) 十六进制前16位 + C`（ESP32 端由 `CFG_TESLA_VIN` 自动推导） |
| Service | `00000211-b2d1-43f0-9b88-960cebf8b91e` |
| Write | `00000212-b2d1-43f0-9b88-960cebf8b91e`（ESP32 → 车，当前未使用） |
| Notify | `00000213-b2d1-43f0-9b88-960cebf8b91e`（车 → ESP32，数据主通道） |
| 帧格式 | 加密帧为长度前缀 + protobuf；旁路帧为 53 字节 `SimPayload` |

## 目录结构

```
TESLA-DASHBOARD/
├── platformio.ini              # PlatformIO 配置（板型与库依赖）
├── include/
│   ├── config.h                # 本仪表应用配置（VIN、UI、板型等）
│   ├── tesla_backend_user_config.h # 将仪表配置传给后端库
│   ├── core/UiModel.h          # 前后端之间的 UI 快照
│   ├── data/MockSource.h       # 仪表演示数据源
│   ├── app/
│   │   ├── AppController.h     # 装配 + 主循环
│   │   └── Dashboard.h         # LVGL UI 实现（IDashboard）
│   ├── ui/IDashboard.h         # UI 接口
│   └── platform/               # 硬件平台层
│       ├── BoardProfile.h      # 板型描述表
│       ├── BoardSelect.h       # 编译期选择板型
│       ├── IDisplay.h          # 屏幕接口
│       └── boards/cyd_2432s028.h
└── src/
    ├── main.cpp                # 组合根：只做装配
    ├── app/AppController.cpp
    ├── app/Dashboard.cpp
    ├── data/MockSource.cpp
    └── display/Display.cpp

TeslaBleBackend/                 # 与 TESLA-DASHBOARD 同级的可复用 PlatformIO 库
```

## 快速开始

```bash
pio run                 # 编译
pio run -t upload       # 烧录（自动找串口）
pio device monitor      # 打开 115200 串口
```

1. 启动模拟器（根目录）：`python sim_server.py`（默认 VIN `LRWYGCFJ8RC571213`，
   换 VIN 加 `--vin`，同时改 `config.h` 的 `CFG_TESLA_VIN`）。
2. 上电后日志流程应为：

```

[0.20s] 目标 VIN=LRWYGCFJ8RC571213  广播名=S<16位hex>C
[0.21s] BLE 协议栈就绪（特斯拉 Central）…
[0.22s] 扫描 BLE 设备 (1 s)...
[1.24s] 找到目标(精确名): S<16位hex>C (aa:bb:…)
[1.60s] BLE 已连接: S…，链路就绪
[1.61s] 特斯拉协议层就绪，等待握手/车辆数据响应
[2.41s] [仪表] BLE:已连接 | 数据:就绪 | 车速 128 km/h | SOC 78% | 续航 312 km | 档位 D | 功率 +44.0 kW
```

> 已验证环境：PlatformIO Core 6.2.0 + espressif32 8.0.0 + Arduino core 3.3.12 + **NimBLE-Arduino 2.5.x**，
> 默认板型 `esp32dev`。⚠️ NimBLE 必须 2.x：1.x 与 core 3.x/IDF5 不兼容，会在
> `NimBLEDevice::init` 处报 `ESP_ERR_INVALID_STATE` 进入重启循环。

## 在其他 ESP32 Arduino 项目中使用后端库

后端库只面向 **ESP32 + Arduino**，BLE 由 NimBLE-Arduino 提供；不依赖 LVGL、
LovyanGFX 或本项目的屏幕代码。

1. 将仓库根目录的 `TeslaBleBackend/` 放入新项目的 `lib/TeslaBleBackend/`，或在
   `platformio.ini` 中用 `lib_extra_dirs` 指向包含 `TeslaBleBackend/` 的目录。
2. 在新项目的 `lib_deps` 中加入 `h2zero/NimBLE-Arduino@^2.5.1`。
3. 建立 `include/tesla_backend_user_config.h`，配置至少 VIN 与客户端私钥，例如：

   ```cpp
   #pragma once
   #define CFG_TESLA_VIN "YOUR_VIN"
   #define CFG_TESLA_CLIENT_KEY_HEX "YOUR_64_HEX_CHAR_PRIVATE_KEY"
   ```

   私钥不要提交到公开仓库。其余 BLE UUID、轮询间隔等参数可在此文件中覆盖。
4. 在程序中创建 `TeslaBleBackend`，在 `setup()` 调用 `begin()`，并在每次 `loop()`
   调用 `loop()`；车辆快照和连接状态通过 `state()`、`status()` 读取。

   ```cpp
   #include <TeslaBleBackend.h>

   TeslaBleBackend vehicle;

   void setup() {
     Serial.begin(115200);
     vehicle.begin();
   }

   void loop() {
     vehicle.loop();
     const VehicleState& car = vehicle.state();
     // 按需将 car.speedKmh、car.soc 等字段显示在目标开发板上
   }
   ```

此目录目前是本仓库内的 PlatformIO 子库；若要通过 PlatformIO Registry 或独立 Git
URL 安装，需要将该目录发布为独立仓库/包。

## 常见调整

- **上电后无限 `Rebooting...`（重启循环）** → 日志里找
  `ESP_ERROR_CHECK failed: ... ESP_ERR_INVALID_STATE ... NimBLEDevice::init`：
  是 NimBLE 库版本问题，`platformio.ini` 的 `lib_deps` 必须是 `h2zero/NimBLE-Arduino@^2.5.1`（1.x 不支持 core 3.x），改完 `pio run -t upload` 重烧
- **模拟器换了 VIN** → `include/config.h` 改 `CFG_TESLA_VIN`（或直接在 `CFG_TESLA_NAME` 写死广播名）
- **连上了但报"未找到服务"** → 核对 `CFG_SERVICE_UUID / CFG_CHAR_WRITE_UUID / CFG_CHAR_NOTIFY_UUID`
- **连上了但一直"尚未收到数据帧"** → 模拟器与 `CFG_TESLA_VIN` 是否一致、Notify 是否订阅成功
- **屏幕方向不对（竖屏显示 / 画面转 90° / 上下颠倒）** →
  `include/config.h` 的 `CFG_LCD_FLUSH_ROTATION`：默认 `90`（横屏）；
  上下颠倒改 `270`。面板初始化参数（`CFG_LCD_ROTATION 1` +
  `CFG_LCD_OFFSET_ROTATION 4`、面板按 320×240 声明）是这块克隆 ILI9341
  实测稳定的组合，**不要修改**，改成原生 240×320 会花屏
- **数据超时判定** → `CFG_DATA_STALE_MS`（默认 3000ms，超时字段显示 `--`）
- **数据字段增删** → `tesla/TeslaPayload.h`（帧定义）+ `VehicleData.h`（快照）+ `Dashboard.cpp`（渲染）

## 路线图

- [x] 工程骨架：分层（链路层 / 协议层 / 数据层 / 渲染层）
- [x] BLE 扫描 + 连接 + 重连 + 通知订阅（精确广播名匹配）
- [x] 特斯拉 53 字节 SimPayload：组帧 / 重同步 / 合理性校验 / 快照写入
- [x] 断连与超时的仪表降级显示（`--`）
- [ ] 接屏（ST7789 圆屏 + LVGL，只改 Dashboard 模块）
- [ ] Write 通道指令下发（如车辆控制，00000212 特征值）
- [x] 真车配对/加密流程（add-key-request 刷卡配钥 + add-key，见 docs/PAIRING.md）
