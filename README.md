# Tesla BLE Simulator (v2.0 Stable Release) 🚗⚡

[![Version](https://img.shields.io/badge/version-2.0%20Stable-brightgreen.svg)](https://github.com/elementchen/Tesla-BLE-Simulator)
[![Platform](https://img.shields.io/badge/platform-Windows%2010%20%7C%2011-blue.svg)](https://github.com/elementchen/Tesla-BLE-Simulator)
[![Python](https://img.shields.io/badge/python-3.10%2B-blue.svg)](https://www.python.org/)
[![License](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)

一个专为“特斯拉 BLE TFT 仪表盘 (ESP32-S3)”打造的 Windows 端特斯拉车机低功耗蓝牙 (BLE) 遥测与控制仿真器。

本仿真器可以在 Windows 10/11 主机上扮演特斯拉官方车机的 Peripheral 外设角色，广播基于 VIN SHA-1 计算的空中 BLE 广播包，提供高拟真度的行车遥测数据灌入，免去连接真实特斯拉车辆的门槛，无线驱动 ESP32 仪表盘的页面跳转、HMI 动画与逻辑状态机。

---

## 🌟 核心特性 (v2.0 Highlights)

* **真车级 BLE 通讯拟真 (Real-Car BLE Pacing)**：
  * **800ms 真实分批推送**：完美模拟特斯拉真实行车 `DriveState` 的 800ms 遥测刷新周期，解决 ESP32 握手卡死与 `SYNC_TIMEOUT_MS = 25000` (25s) 超时复位问题。
  * **网络抖动与延迟模拟**：包含 150ms ~ 300ms 拟真响应延迟、5% 概率长尾链路抖动。
  * **丢包率与故障注入**：支持 0% ~ 20% 动态丢包率测试，以及一键 5 秒 BLE 信号闪断/断连故障注入。
  * **双通信模式无缝切换**：支持 **`问答/分批模式 (REALISTIC 800ms)`** 与 **`理想主动推送 (IDEAL 20Hz)`** 随时切换。
* **硬件级 Bumble USB HCI 蓝牙直控 (CSR8510 Dongle)**：
  * 基于 `Bumble` + `WinUSB` 原生 USB 接口直控（绕过 Windows UWP 蓝牙限制），支持普通 15 元 USB 蓝牙接收器物理广播。
  * 动态 VIN 计算：用户输入 17 位 VIN 码后，实时计算 `S<sha1_prefix>C` 标准特斯拉空中广播包名称。
  * 硬件级广播总开关（`BLE广播: ON / OFF`）与连接状态显示。
* **可视化暗黑风 Tkinter 操控台**：
  * **核心驾控参数**：车速（0~200 km/h）、电机功率（-100~300 kW，带能量回收进度条高亮）、电池 SOC（0~100%）、续航里程（KM）、PRND/SNA 档位控制。
  * **车身与充电**：4 门 + 前后备箱开口开闭、中控锁锁定、充电枪插拔与充电功率调节（0~250 kW）。
  * **环境与胎压监测**：车内/车外温度控制、四轮实时胎压 (TPMS，带异常红字告警)。
  * **自动化剧本播放引擎**：内置 9 阶段经典行车闭环剧本（解锁上车 -> 挂档加速 -> 强力超车 -> 动能回收 -> 倒车入库 -> 插枪充电），支持一键脚本自动播放。

---

## 🖥️ 界面展示


<img width="1122" height="852" alt="20260819194236_17_341" src="https://github.com/user-attachments/assets/5563c86b-5161-4524-87c0-1277899a0880" />

---

## 🚀 极速安装与使用指南

### 1. 硬件准备（强烈推荐使用 USB 蓝牙接收器）
为了获得最真实的物理层空中广播体验，建议准备一个低成本（约 10~15 元）的 **CSR8510 A10 USB 蓝牙接收器**。
按说明文档配置 Zadig WinUSB 驱动：
👉 **[普通 USB 蓝牙接收器配置指南 (docs/USB_BLUETOOTH_DONGLE_SETUP.md)](docs/USB_BLUETOOTH_DONGLE_SETUP.md)**

### 2. 方式一：一键批处理启动 (Windows 推荐)
直接在资源管理器中双击：
👉 **`run.bat`**

（脚本会自动检查并安装 `requirements.txt` 依赖，以管理员模式拉起仿真服务）

### 3. 方式二：终端命令行启动
打开 PowerShell 或 CMD，在当前目录运行：

```powershell
# 1. 安装核心依赖
pip install -r requirements.txt

# 2. 启动图形化仿真服务
python sim_server.py

# (可选) 强制使用 Bumble CSR8510 USB HCI 模式
python sim_server.py --bumble

# (可选) 纯命令行模式 (无 GUI 界面)
python sim_server.py --nogui --vin LRWYGCFJ8RC571213
```

---

## 🗃️ 协议规范：`SimPayload` (53 Bytes)

本模拟器与 ESP32 仪表端采用定制的紧凑二进制字节包进行高频通信，单包大小固定为 **53 字节**。ESP32NimBLE 接收层在 `ble_adapter.cpp` 中感知到 53 字节长度后，会自动跳过 ECDH 握手与 Protobuf 解密，直接打包传入 UI 渲染流。

Python 侧使用 `struct.pack("<ffifc6BBfff4f", ...)` 编码，结构对齐如下：

| 偏移量 (Byte) | 字段名称 | 数据类型 | 作用描述 |
| :--- | :--- | :--- | :--- |
| `0 - 3` | `speed_kmh` | `float` | 实时时速 (KM/H) |
| `4 - 7` | `motor_power_kw` | `float` | 电机输出功率 (正值为输出，负值代表能量回收) |
| `8 - 11` | `battery_level` | `int32_t` | 剩余电量 SOC (%) |
| `12 - 15` | `battery_range_km` | `float` | 估计续航里程 (KM) |
| `16` | `gear` | `char` | 当前档位 (`P`, `R`, `N`, `D`, `?`巡航) |
| `17 - 22` | `doors[6]` | `uint8_t[6]` | 车身开口状态 (fl, fr, rl, rr, frunk, trunk) |
| `23` | `locked` | `uint8_t` | 整车中控锁定状态 (0解锁, 1锁定) |
| `24` | `charging` | `uint8_t` | 充电枪连接状态 (0未充电, 1充电中) |
| `25 - 28` | `charge_power_kw` | `float` | 实时充电功率 (kW) |
| `29 - 32` | `inside_temp` | `float` | 车内环境温度 (℃) |
| `33 - 36` | `outside_temp` | `float` | 车外环境温度 (℃) |
| `37 - 52` | `tpms[4]` | `float[4]` | 四轮实时胎压 (fl, fr, rl, rr) |

---

## 📚 文档目录 (Docs Directory)

更多架构设计与硬件指南请参考 `docs/` 文件夹：
* 📖 **[USB_BLUETOOTH_DONGLE_SETUP.md](docs/USB_BLUETOOTH_DONGLE_SETUP.md)**：普通 USB 蓝牙接收器 (CSR8510) + Zadig WinUSB 驱动一步一步配置指南。
* 📖 **[HANDOVER.md](docs/HANDOVER.md)**：模拟器架构设计、旁路劫持原理与二次开发指南。

---

## 📜 开源协议

本项目采用 **MIT License** 开源协议。欢迎提交 Pull Request 和 Issue！
