# ESP32 BLE Tesla Dashboard

基于 ESP32 的车载仪表盘：ESP32 作为 **BLE 中心设备（Central/Client）** 扫描并连接
**特斯拉 BLE**（模拟器 `sim_server.py` 或真车），订阅 Notify 通道接收 53 字节
`SimPayload` 遥测帧，解析成统一的数据快照后由仪表层渲染（当前用串口模拟，后续接屏）。

> **数据源只有特斯拉 BLE**，OBD/ELM327 路径不再使用（`src/obd` 已通过
> `platformio.ini` 的 `build_src_filter` 排除编译）。

## 技术选型

| 项 | 选择 | 说明 |
| --- | --- | --- |
| 框架 | PlatformIO + Arduino | 生态成熟，库管理方便 |
| 蓝牙库 | NimBLE-Arduino | 比 Bluedroid 省 200KB+ 内存 |
| BLE 角色 | Client（连车端） | 扫描（精确广播名） → 连接 → 订阅通知 |
| 协议 | 53B 定长二进制帧 | 车端 800ms 节拍主动推送，无需请求 |
| 显示 | 暂无，串口模拟表盘 | 屏幕驱动后加，只改 Dashboard 模块 |

## 架构

```
特斯拉模拟器 / 真车
     │  BLE Notify（800ms 节拍，53 字节 SimPayload）
     ▼
┌────────────┐  字节   ┌──────────────┐  组帧   ┌──────────────┐   渲染   ┌────────────┐
│  BleLink   │ ──────► │ TeslaClient  │ ──────► │  g_vehicle   │ ───────► │ Dashboard  │
│ 扫描/连接/  │  队列   │ 53B组帧/重同步 │  解析   │ VehicleData  │          │ 串口模拟，  │
│ 订阅通知    │         │ 合理性校验    │        │ (唯一数据源) │          │ 后续换 TFT │
└────────────┘         └──────────────┘         └──────────────┘          └────────────┘
     ▲                                                     ▲
     └──────────────── config.h 全局配置 ──────────────────┘
```

- **BleLink**（链路层）：扫描 → 连接 → 订阅，收到的字节只入环形缓冲，loop 里再交给上层。
- **TeslaClient**（协议层）：字节累积成 53 字节帧，逐字节重同步容忍 MTU 拆包；
  合理性校验通过后写入 `g_vehicle`；超时（`CFG_DATA_STALE_MS`）后仪表降级显示 `--`。
- **VehicleData**（数据层）：全局唯一数据源 `g_vehicle`，`valid` 位 + `fresh()` 过期判断。
- **Dashboard**（渲染层）：接屏时只改这一个模块。

## 协议要点

| 项 | 值 |
| --- | --- |
| 广播名 | `S + sha1(VIN) 十六进制前16位 + C`（ESP32 端由 `CFG_TESLA_VIN` 自动推导） |
| Service | `00000211-b2d1-43f0-9b88-960cebf8b91e` |
| Write | `00000212-b2d1-43f0-9b88-960cebf8b91e`（ESP32 → 车，当前未使用） |
| Notify | `00000213-b2d1-43f0-9b88-960cebf8b91e`（车 → ESP32，数据主通道） |
| 帧格式 | 53 字节，`struct.pack("<ffifc6BBfff4f")`，见根目录 README 协议表 |

## 目录结构

```
TESLA-DASHBOARD/
├── platformio.ini          # PlatformIO 工程配置（板子/库依赖，obd 已排除编译）
├── include/
│   ├── config.h            # 所有可调参数：VIN、广播名、UUID、超时…
│   ├── ble/BleLink.h       # BLE 链路层：精确名扫描/连接/订阅/收发
│   ├── tesla/TeslaPayload.h# 53 字节 SimPayload 帧定义（packed + static_assert）
│   ├── tesla/TeslaClient.h # 特斯拉协议层：组帧/校验/写入快照/广播名推导
│   ├── data/VehicleData.h  # 车辆数据快照（全局唯一数据源）
│   └── app/Dashboard.h     # 仪表渲染层（当前串口模拟）
└── src/
    ├── main.cpp            # 组装：setup / loop（VIN → 广播名 → 链路）
    ├── ble/BleLink.cpp
    ├── tesla/TeslaClient.cpp
    ├── data/VehicleData.cpp
    └── app/Dashboard.cpp
    （obd/ 保留但不编译）
```

## 快速开始

```bash
pio run                 # 编译
pio run -t upload       # 烧录（自动找串口）
pio device monitor      # 打开 115200 串口
```

1. 启动模拟器（根目录）：`python sim_server.py`（默认 VIN `5YJ3E1EA0KF000000`，
   换 VIN 加 `--vin`，同时改 `config.h` 的 `CFG_TESLA_VIN`）。
2. 上电后日志流程应为：

```
[0.20s] 目标 VIN=5YJ3E1EA0KF000000  广播名=S<16位hex>C
[0.21s] BLE 协议栈就绪（特斯拉 Central）…
[0.22s] 扫描 BLE 设备 (10 s)...
[1.35s] 找到目标(精确名): S<16位hex>C (aa:bb:…)
[1.60s] BLE 已连接: S…，链路就绪
[1.61s] 特斯拉协议层就绪，等待 53 字节 SimPayload 帧
[2.41s] [仪表] BLE:已连接 | 数据:就绪 | 车速 128 km/h | SOC 78% | 续航 312 km | 档位 D | 功率 +44.0 kW
```

> 已验证环境：PlatformIO Core 6.2.0 + espressif32 8.0.0 + Arduino core 3.3.12 + NimBLE-Arduino 1.4.3，
> 默认板型 `esp32dev`。

## 常见调整

- **模拟器换了 VIN** → `include/config.h` 改 `CFG_TESLA_VIN`（或直接在 `CFG_TESLA_NAME` 写死广播名）
- **连上了但报"未找到服务"** → 核对 `CFG_SERVICE_UUID / CFG_CHAR_WRITE_UUID / CFG_CHAR_NOTIFY_UUID`
- **连上了但一直"尚未收到数据帧"** → 模拟器与 `CFG_TESLA_VIN` 是否一致、Notify 是否订阅成功
- **数据超时判定** → `CFG_DATA_STALE_MS`（默认 3000ms，超时字段显示 `--`）
- **数据字段增删** → `tesla/TeslaPayload.h`（帧定义）+ `VehicleData.h`（快照）+ `Dashboard.cpp`（渲染）

## 路线图

- [x] 工程骨架：分层（链路层 / 协议层 / 数据层 / 渲染层）
- [x] BLE 扫描 + 连接 + 重连 + 通知订阅（精确广播名匹配）
- [x] 特斯拉 53 字节 SimPayload：组帧 / 重同步 / 合理性校验 / 快照写入
- [x] 断连与超时的仪表降级显示（`--`）
- [ ] 接屏（ST7789 圆屏 + LVGL，只改 Dashboard 模块）
- [ ] Write 通道指令下发（如车辆控制，00000212 特征值）
- [ ] 真车配对/加密流程（见 docs/ENCRYPTION_SIMULATION_PLAN.md）
