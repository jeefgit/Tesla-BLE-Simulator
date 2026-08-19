# Implementation Plan - Introduce SNA Gear and Tesla BLE Reference Alignment

基于对特斯拉官方 BLE 通信协议定义（[tesla_ble_data_reference.md](file:///E:/AI_coding_test/_Gemini/TESLA_BLE_TFT/doc/tesla_ble_data_reference.md)）和 C++ 端提取逻辑的深度对齐，我们需要在模拟器 GUI 中引入“SNA（辅助驾驶/信号未指定）”档位，并打通其对应的遥测数据通路。

## 🎯 SNA 档位技术分析

在特斯拉的 [vehicle.proto](file:///e:/AI_coding_test/_Gemini/TESLA_BLE_TFT/components/tesla-ble/proto/vehicle.proto) 特斯拉车辆协议定义中，档位枚举 `ShiftState` 定义如下：
- `P`/`R`/`N`/`D`/`S` (Sport) 以及 **`SNA` (Signal Not Available)**。
- 对应到我们 ESP32 仪表的适配层 [main.cpp](file:///e:/AI_coding_test/_Gemini/TESLA_BLE_TFT/main/main.cpp#L51-L60) 中的档位转换函数 `gear_from_shift_state`：
  ```cpp
  static char gear_from_shift_state(const CarServer_ShiftState &ss) {
      switch (ss.which_type) {
          case CarServer_ShiftState_P_tag:   return 'P';
          case CarServer_ShiftState_R_tag:   return 'R';
          case CarServer_ShiftState_N_tag:   return 'N';
          case CarServer_ShiftState_D_tag:   return 'D';
          case CarServer_ShiftState_SNA_tag: return '?'; // SNA 映射为字符 '?'
          default:                           return '-';
      }
  }
  ```
- **结论**：**特斯拉的 BLE 传输中确实有 SNA 档，在底层的传输协议和我们 C++ HMI 提取层中，它对应并传输单字符 `'?'`。** 

当模拟器将 `'?'` 作为档位字符发送给仪表盘时，ESP32 仪表盘会自动判定进入 AP (Autopilot) 辅助驾驶激活或定速巡航状态。

## Proposed Changes

### Simulation App Component

#### [MODIFY] [sim_server.py](file:///e:/AI_coding_test/_Gemini/TESLA_BLE_TFT/Simulation_app/sim_server.py)
对 `sim_server.py` 的 GUI 和状态映射逻辑进行修改，将 `SNA` 按钮完美融入车辆控制面板：
1. **控制面板 `setup_body_controls`**：
   - 将档位按钮数组从 `['P', 'R', 'N', 'D']` 扩展为 `['P', 'R', 'N', 'D', 'SNA']`。
   - `SNA` 按钮宽度稍微放宽，以便能够显示三个字符的英文。
2. **事件映射逻辑 `set_gear_state`**：
   - 增加界面与协议的映射：当用户点击 `SNA` 按钮时，将数据集中存储的 `gear` 设为 `'?'` 字符，确保在打包为 1 字节 `char` 时不会发生长度溢出崩溃。
3. **按钮指示同步 `refresh_gear_ui`** 与 `on_btn_hover_leave`：
   - 当接收到当前激活档位为 `'?'` 时，将界面上的 `SNA` 按钮以特斯拉红色高亮显示，其余灰色低亮；当离开 Hover 时，根据状态机内的 `'?'` 正确恢复 `SNA` 的高亮。

## Verification Plan

### Automated Tests
1. **启动验证**：
   ```powershell
   python sim_server.py
   ```
   启动 GUI，确认顶栏的 VIN 和蓝牙就绪后，检查车身卡片的挡位处是否新增了 `SNA` 按钮，排版没有错位。

### Manual Verification
1. **档位切换打包验证**：
   - 在 GUI 顶栏切换为“手动干预模式”。
   - 点击挡位组的 `SNA`，查看日志区输出的 `[BLE-Notify]` 日志，其 `Gear` 属性是否变为 `?`，系统没有抛出 struct 打包异常。
   - 点击 `D`，查看日志中的 `Gear` 是否恢复为 `D`。
2. **ESP32 AP 界面同步验证**：
   - 用 ESP32 连接此仿真器，在 GUI 上点击 `SNA`。
   - 观察 ESP32 仪表的档位显示是否显示为 AP 状态或特定的巡航蓝色图标；点击 `P` 观察其是否顺利切回驻车挡。
