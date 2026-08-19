# Tesla BLE Simulator - 历史对话与开发全景记录 (Conversation & Dev History Summary)

本文档整理并汇总了从项目初始到 `v2.0 稳定版` 历次与 AI 协同开发的完整对话背景、技术决策链路与故障排查经验，供后续接手开发（如 Claude Agent 或人类开发者）快速了解全貌。

---

## 📌 项目关键演进脉络

### 1. 初始阶段：Windows 蓝牙模拟与 53B 旁路协议
* **目标**：在 Windows 上模拟特斯拉车机 BLE Peripheral 广播，驱动 ESP32 仪表盘。
* **设计决策**：因实车 ECDH（X25519）私钥协商和 AES-GCM-128 加密协议在 Windows 平台实现过于冗长复杂，系统在 ESP32 固件端（`ble_adapter.cpp`）植入了 **53 字节 `SimPayload` 长度自适应旁路**。当 ESP32 收到 53 字节数据包时，直接解包渲染，跳过解密流程。
* **初期形式**：Python 脚本以 20Hz 理想高频死循环推送。

### 2. 硬件层演进：Bumble USB HCI 物理直控替代 Windows BLE
* **问题**：Windows 原生 UWP / WinRT BLE API 限制极大，不允许任意指定基于 VIN SHA-1 计算的特斯拉广播名称 `S<sha1_prefix>C`，且无法稳定控制物理广播。
* **突破方案**：引入普通 10~15 元 **CSR8510 A10 USB 蓝牙接收器**，使用 **Zadig** 将驱动替换为 **WinUSB**，并在 Python 侧通过 Google **Bumble** 库直接通过 USB 物理接口（HCI 端口 `0a12:0001`）发包。
* **成果**：彻底摆脱 Windows 蓝牙协议栈限制，实现真正的 100% 特斯拉物理广播与双卡独立工作。

### 3. 拟真度升级与 25秒卡死超时 (SYNC TIMEOUT) 根因排查
* **现象**：实车测试与模拟器测试表现不一致；模拟器曾尝试改用“完全等待 ESP32 请求后再回复”的问答模式，导致 ESP32 发生严重卡顿并每 25 秒自动断连复位。
* **深度根因（ESP32 Agent 协同诊断）**：
  - ESP32 发出的 BLE Write 请求是经过 ECDH 加密的 Protobuf 报文。
  - 模拟器收到后无法解密故未予回复，导致 ESP32 固件等待 Session 握手超时，触发了 `static constexpr uint32_t SYNC_TIMEOUT_MS = 25000;`（25秒超时自动擦除秘钥并复位重来）。
* **终极解决方案（方案 A：真车节奏分批推送）**：
  - 模拟器保持 53 字节明文 `SimPayload` 格式（满足 ESP32 明文绕过条件，彻底消除 25s 超时）。
  - 推送机制从“20Hz 盲目刷屏”改为“符合特斯拉 `DriveState` 的 **800ms 基础周期分批推送**”。
  - 引入拟真延时滑块（0.2x~3.0x）、5% 长尾链路抖动、丢包率控制（0~20%）以及一键 5 秒信号闪断故障注入。

### 4. 界面与产品化：Tkinter 暗黑风控制台
* 实现了车速（0~200 km/h）、电机功率（动能回收高亮）、电量 SOC、续航、档位（PRND/SNA）、4 门 + 2 备箱、中控锁、充电枪（功率可调）、内外温与四轮胎压滑块。
* 内置 9 阶段经典行车剧本自动播放。
* 提供一键 BLE 广播总开关（ON/OFF）及实时延迟监控。

### 5. v2.0 稳定版发布与开源准备
* **隐私去隐化**：移除代码中所有个人真实车架号和蓝牙物理 MAC，替换为开源标准示例 `5YJ3E1EA0KF000000` / `F4:4E:FC:00:00:01`，由用户前端动态输入 VIN 生成广播名。
* **硬件指南**：编写详细图文文档 [docs/USB_BLUETOOTH_DONGLE_SETUP.md](USB_BLUETOOTH_DONGLE_SETUP.md)。
* **GitHub 仓库**：成功推送到开源仓库 [https://github.com/elementchen/Tesla-BLE-Simulator](https://github.com/elementchen/Tesla-BLE-Simulator) 并标记 `v2.0` Tag。

---

## 🗂️ 附带完整原始对话归档

原始全量对话记录已保存在本目录下：
* **[docs/conversation_logs/walkthrough.md](conversation_logs/walkthrough.md)** (v2.0 发布记录)
* **[docs/conversation_logs/implementation_plan.md](conversation_logs/implementation_plan.md)** (实现设计文档)
