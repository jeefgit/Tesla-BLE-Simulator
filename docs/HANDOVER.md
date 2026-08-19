# Tesla BLE TFT HMI Windows Simulator - Handover Guide (项目移交与架构指南)

致接手本项目的 AI Agent 伙伴与开发者：

欢迎接手 **Tesla BLE Simulator (Simulation_app)** 项目！本项目的目标是为 ESP32 车规级仪表盘提供一个 100% 模拟真实特斯拉车机的低功耗蓝牙 (BLE) 数据灌入源，免去连接真实车辆的门槛，兼具 **20Hz 理想主动推送模式** 与 **800ms 真车节奏分批推送模式**，无线驱动仪表的 HMI 动画与逻辑状态机。

---

## 🧭 项目架构与数据流向

本模拟器运行在 Windows 主机侧，扮演低功耗蓝牙 (BLE) 外设（Peripheral）角色，与扮演中心（Central）角色的 ESP32 仪表进行无线通信：

```mermaid
graph LR
    subgraph Windows Host (Simulation_app)
        A[sim_server.py] -->|1. GUI / Script Data| B[TelemetryState]
        B -->|2. Pack to SimPayload| C[struct.pack format]
        C -->|3. BLE Notify| D[Bumble USB HCI Server / Bless]
    end
    D == Wireless BLE (CSR8510 USB Dongle) ==> E[ESP32 NimBLE Central]
    subgraph ESP32 (Firmware Side)
        E -->|4. Intercept 53-byte Pack| F[ble_adapter.cpp]
        F -->|5. Bypass Decryption| G[main.cpp Loop]
        G -->|6. Render HMI| H[LVGL Drive/Charge UI]
    end
```

### 🗝️ 为什么不走实车的 ECDH 和 AES-GCM 加密？
真实的特斯拉车机通信包含复杂的私钥加载、ECDH（X25519）共享密钥协商以及 AES-GCM-128 加密认证。
为了在 Windows 上免去复杂加密握手的限制，系统在 ESP32 适配层 [ble_adapter.cpp](file:///e:/AI_coding_test/_Gemini/TESLA_BLE_TFT/main/ble_adapter.cpp) 植入了**自适应长度旁路劫持**：
* 当接收到的蓝牙数据包长度**正好为 53 字节**时，ESP32 判定其为本仿真器发出的裸包（`SimPayload`），直接解包渲染，**跳过解密握手与 25 秒超时删密钥逻辑**！这极大地方便了 HMI 界面的无线开发与参数调试。

---

## 📡 BLE 通信与协议规范

### 1. 广播服务与特征值定义
模拟器在空中广播以下特斯拉特定 UUID 属性：
* **Service UUID**: `00000211-b2d1-43f0-9b88-960cebf8b91e`
* **Write Characteristic UUID** (用于接收 ESP32 命令/指令): `00000212-b2d1-43f0-9b88-960cebf8b91e` (Permissions: Writeable)
* **Notify Characteristic UUID** (数据发送主通道): `00000213-b2d1-43f0-9b88-960cebf8b91e` (Properties: Notify)

### 2. 应用层数据封包：`SimPayload`
数据包为紧凑的二进制对齐结构体，总长度为 **53 字节**。
在 Python 侧通过 `struct.pack(PAYLOAD_FORMAT, ...)` 打包，格式字符串为：
```python
PAYLOAD_FORMAT = "<ffifc6BBfff4f"
```

字段在内存中的排布与 C++ 的 [sim_payload.h](file:///e:/AI_coding_test/_Gemini/TESLA_BLE_TFT/main/sim_payload.h) 结构体严格保持一致：

| 偏移量 | 字段名称 | 类型 (Python -> C++) | 作用描述 |
| :--- | :--- | :--- | :--- |
| `0 - 3` | `speed_kmh` | `float` -> `float` | 实时车速数字 (KM/H) |
| `4 - 7` | `motor_power_kw` | `float` -> `float` | 电机输出功率 (负数代表能量回收进度条高亮) |
| `8 - 11` | `battery_level` | `int` -> `int32_t` | 剩余电量 SOC (%) |
| `12 - 15` | `battery_range_km` | `float` -> `float` | 估计续航距离 (KM) |
| `16` | `gear` | `char` -> `char` | 当前档位 (`P`/`R`/`N`/`D`/`?`巡航) |
| `17 - 22` | `doors[6]` | `6B` -> `uint8_t[6]` | 车身开口状态 (fl, fr, rl, rr, frunk, trunk) |
| `23` | `locked` | `B` -> `uint8_t` | 中控锁定状态 (0解锁, 1锁定) |
| `24` | `charging` | `B` -> `uint8_t` | 充电枪连接状态 (0未充电, 1充电中) |
| `25 - 28` | `charge_power_kw` | `float` -> `float` | 充电实时功率 (kW) |
| `29 - 32` | `inside_temp` | `float` -> `float` | 车内空气温度 (℃) |
| `33 - 36` | `outside_temp` | `float` -> `float` | 车外环境温度 (℃) |
| `37 - 52` | `tpms[4]` | `4f` -> `float[4]` | 四轮实时胎压 (fl, fr, rl, rr) |

---

## 📂 代码库文件清单

`Simulation_app` 目录中的核心文件及其职责如下：
* **[sim_server.py](file:///e:/AI_coding_test/_Gemini/TESLA_BLE_TFT/Simulation_app/sim_server.py)**：仿真核心脚本。包含 Tkinter 暗黑 UI 面板、Bumble USB HCI 服务端、800ms 真车节奏/理想 20Hz 双模式推送逻辑、丢包与故障注入控制。
* **[docs/USB_BLUETOOTH_DONGLE_SETUP.md](file:///e:/AI_coding_test/_Gemini/TESLA_BLE_TFT/Simulation_app/docs/USB_BLUETOOTH_DONGLE_SETUP.md)**：普通 USB 蓝牙接收器 (CSR8510) + Zadig WinUSB 驱动配置指南。
* **[requirements.txt](file:///e:/AI_coding_test/_Gemini/TESLA_BLE_TFT/Simulation_app/requirements.txt)**：声明依赖库（`bumble`, `libusb-package`, `bless`）。
* **[run.bat](file:///e:/AI_coding_test/_Gemini/TESLA_BLE_TFT/Simulation_app/run.bat)**：一键管理员运行脚本。
* **[README.md](file:///e:/AI_coding_test/_Gemini/TESLA_BLE_TFT/Simulation_app/README.md)**：用户使用说明文档。

---

## 🛠️ 已完成功能与后续扩展建议 (Roadmap)

### ✅ 已完成功能 (v2.0)
1. **Tkinter 暗黑风图形化操控台**：已完成手动滑块/按键干预车速、档位、车门、电量、胎压、温度等全套 UI 参数。
2. **真车 BLE 节奏模拟**：支持 800ms 基础间隔分批推送、150ms~300ms 拟真延时、5% 长尾链路抖动、0~20% 丢包率模拟、一键 5 秒信号闪断故障注入。
3. **Bumble USB HCI 物理广播**：支持 CSR8510 Dongle 物理收发与动态 VIN 广播名刷新（`S<sha1_prefix>C`）。

### 🎯 建议扩展方向
1. **扩展车机状态报文**：在 `SimPayload` 中扩展转向灯（左/右/双闪）、远光灯、AP 自动辅助驾驶状态图标等。
2. **自动化测试集与 Log 回放**：支持读取车机串口/蓝牙 Capture Log 并在模拟器中进行帧级别回放测试。
