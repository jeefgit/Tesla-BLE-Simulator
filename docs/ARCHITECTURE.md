# 前后端分离架构（ESP32 固件）

> 目标：换 ESP32 板子、换屏幕、换数据源时，**不修改业务代码**。
> 适用范围：`TESLA-DASHBOARD/`（ESP32 固件）。PC 端 `sim_server.py` 不在本方案内。

## 1. 分层

| 层 | 目录 | 职责 | 依赖 |
|---|---|---|---|
| 领域模型 | `include/core/` | `VehicleState` / `LinkState` / `PairingHint` / `SourceStatus` / `UiModel` | 仅标准库 + Arduino 基础类型 |
| 后端数据源 | `include/data/`、`src/data/` | `IVehicleSource` 及其实现（Tesla BLE / Mock / OBD） | core、协议实现 |
| 协议实现 | `src/tesla/`、`src/ble/` | 特斯拉加密、配钥、GATT 链路 | 仅被 `TeslaBleSource` 调用 |
| 应用装配 | `include/app/AppController.h` | 组合数据源 + UI + 屏幕，组装 `UiModel` | 全部接口 |
| 前端 UI | `include/app/Dashboard.h`、`src/app/Dashboard.cpp`、`include/ui/` | `IDashboard` 实现，只读 `UiModel` | core + LVGL |
| 平台/硬件 | `include/platform/`、`src/display/` | `IDisplay`、`BoardProfile`、板型表 | LovyanGFX / LVGL |

数据流：

```
IVehicleSource::loop()
        │
        ▼
   state() + status()  ──►  UiModel  ──►  IDashboard::render()  ──►  LVGL
                                                    │
                                                    ▼
                                            IDisplay::loop()
```

**关键约束**：UI 代码不得包含 `ble/`、`tesla/`、`platform/` 头文件；后端不得包含 `lvgl.h`。

## 2. 接口

```cpp
// 后端
class IVehicleSource {
  virtual void begin() = 0;
  virtual void loop() = 0;
  virtual const VehicleState& state() const = 0;
  virtual SourceStatus status() const = 0;
};

// 前端
class IDashboard {
  virtual void begin() = 0;
  virtual void render(const UiModel& model) = 0;
};

// 屏幕
class IDisplay {
  virtual bool begin(const BoardProfile& board) = 0;
  virtual void loop() = 0;
};
```

## 3. 换数据源

1. 新建 `include/data/XxxSource.h`，实现 `IVehicleSource`。
2. 在 `src/main.cpp` 的编译期分支里选择它（当前是 `CFG_DEMO_MODE` 二选一）。
3. UI 不需要任何修改。

示例：`MockSource` 只把 `status()` 固定为 `LinkState::Ready`，其余与 BLE 完全一致。

## 4. 换板子 / 换屏幕

### 4.1 新增板型

复制 `include/platform/boards/cyd_2432s028.h`，按新板实测值填写 `BoardProfile`：
引脚、面板尺寸、驱动芯片、旋转、反色、RGB 顺序、刷新缓冲行数。

### 4.2 选择板型

在 `include/platform/BoardSelect.h` 增加分支：

```cpp
#elif defined(BOARD_ESP32S3_4848S040)
#include "platform/boards/esp32s3_4848s040.h"
#define BOARD_SELECTED BOARD_ESP32S3_4848S040
```

### 4.3 新增 PlatformIO env

```ini
[env:esp32s3_4848]
platform = espressif32
board = esp32-s3-devkitc-1
framework = arduino
build_flags = ${env.build_flags} -DBOARD_ESP32S3_4848S040
```

> 注意：`CFG_LCD_DRIVER_ILI9341` 目前在 `config.h` 里是全局编译期开关，
> 多板型并存时建议把它也移到各自的 board 头文件并用 `build_flags` 选择。

### 4.4 屏幕分辨率不同时

`Dashboard` 目前是 320×240 绝对坐标布局。新增分辨率时：

1. 新增 `include/ui/layout_480x320.h`（或按断点拆分布局）；
2. 让 UI 的坐标/字号从布局配置读取，而不是写死；
3. 在 `main.cpp` 里按板型选择对应的 `IDashboard` 实现。

## 5. 编译期 vs 运行期

当前按你的要求采用**编译期选择**：
- 数据源：`CFG_DEMO_MODE`；
- 板型：`BOARD_*` 宏。

运行期切换（按键/菜单）可以在 `AppController` 里加一个 `IVehicleSource*` 切换函数，
但 LVGL + BLE 同时驻留会显著增加 RAM 占用，需要先做内存评估。

## 6. 回归检查清单

- [ ] `pio run` 通过，Flash/RAM 无明显增长（当前约 Flash 31.6% / RAM 28.6%）
- [ ] 演示模式（`CFG_DEMO_MODE=1`）能显示 9 阶段剧本
- [ ] BLE 模式能扫描、握手、显示仪表页
- [ ] 断连遮罩、配钥卡片、告警条行为与改造前一致
- [ ] 换板后检查：白屏（引脚/片选）、花屏（面板尺寸/offset）、红蓝互换（rgb_order）、反色（invert）
