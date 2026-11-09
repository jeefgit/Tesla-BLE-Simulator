# Tesla BLE HMI Windows Simulator Server v2.0 稳定版发布说明

## 📌 版本概述 (v2.0 Stable Release)

`v2.0` 是专门为 **Tesla BLE TFT 仪表盘** 打造的真车级 Windows 仿真模拟器稳定版本。该版本整合了跨平台 Pure Python Bumble USB HCI 蓝牙驱动、真车节奏分批推送机制、拟真延时/抖动/丢包控制以及全套 GUI 手动/剧本操控面板。

---

## 🔥 核心功能特性

### 1. 真车级 BLE 通讯拟真 (Real-Car BLE Emulation)
* **节奏分批推送**：符合特斯拉 `DriveState` 的 800ms 真实行车推送周期，告别高频无脑刷屏。
* **拟真延时与抖动**：内置 150ms ~ 300ms 动态响应延时与 5% 长尾链路抖动。
* **丢包与故障注入**：提供丢包率控制（0%~20%）及一键 5s 蓝牙信号闪断测试。
* **双模式切换**：支持在 **`问答/分批模式 (REALISTIC)`** 与 **`理想主动推送 (IDEAL 20Hz)`** 间自由切换。

### 2. 硬件级 Bumble USB HCI 蓝牙驱动 (CSR8510 Dongle)
* **USB 适配器直控**：通过 `bumble` + `libusb` 绕过 Windows UWP BLE 堆栈，使用 CSR8510 Dongle 进行物理广播。
* **动态广播重拉与 VIN 切换**：支持实时修改 17 位 VIN 码并自动计算匹配 SHA-1 广播名称（如 `Sa7785a96101d0c3fC`）。
* **广播物理开关**：提供 `BLE广播: ON / OFF` 控制，支持平滑关闭与开启 USB 蓝牙接口。

### 3. 可视化暗黑风 Tkinter 操控台
* **驾控核心参数**：车速（0~200 km/h）、电机功率（-100~300 kW）、电池 SOC（0~100%）、续航里程、PRND/SNA 档位控制。
* **车身与充电**：4 门 + 2 盖开闭、中控锁、充电枪插拔与充电功率调节（0~250 kW）。
* **环境与胎压**：车内/车外温度、四轮胎压（带异常红字告警）。
* **自动化剧本播放**：内置 9 阶段经典驾驶循环剧本，支持一键切换。

---

## 🛠️ 变动历史与 Commit 记录

- **`e245d42` (Tag: `v2.0`)**: *Simulator side: Release V2.0 Stable Version with Real-Car BLE Emulation*
- **`c82c36a`**: *Fix NameError on req_queue in disconnected branch causing instant launch exit*
- **`4607a8c`**: *Fix uninitialized loop variable in sim_server.py*
- **`e09abe1`**: *Adopt Option A (Intermittent Batched Push Mode) as requested by ESP32 agent*
- **`b0f8e8d`**: *Calibrate real-car emulation base latency to realistic 150-300ms range*
- **`8e2fdea`**: *Fix USB transport release on stop() and disable auto_restart to prevent HCI errors*
- **`cfb2508`**: *Add BLE ON/OFF toggle switch button to top control panel*

---

## 🚀 启动与使用方式

在 `Simulation_app` 目录执行：

```powershell
python sim_server.py
```

或双击使用一键批处理：
```powershell
.\run.bat
```
