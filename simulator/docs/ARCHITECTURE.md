# 前后端分离架构（ESP32 固件）

> 目标：换 ESP32 板子、换屏幕、换数据源时，**不修改业务代码**。
> 适用范围：`TESLA-DASHBOARD/`（ESP32 Arduino 固件）。PC 端 `sim_server.py` 不在本方案内。

## 1. 分层

| 层 | 目录 | 职责 | 依赖 |
|---|---|---|---|
| 领域模型 | `TeslaBleBackend/src/core/` | `VehicleState` / `LinkState` / `PairingHint` / `SourceStatus` | Arduino 基础类型 |
| 后端依赖库 | `TeslaBleBackend/` | `IVehicleSource`、`TeslaBleSource`、BLE 链路、Tesla 加密与配钥协议 | ESP32 Arduino、NimBLE-Arduino |
| 应用数据源 | `src/data/` | 仪表演示数据源 `MockSource` | 后端公开模型 |
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
Tesla BLE 后端以 PlatformIO 本地库形式提供；其他显示/开发板工程可只依赖该库，
但当前实现仍要求 ESP32 Arduino 与 NimBLE-Arduino。

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

1. 新建 `src/data/XxxSource.h`，实现后端库公开的 `IVehicleSource`。
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

当前按**编译期选择**：
- 数据源：`CFG_DEMO_MODE`；
- 板型：`BOARD_*` 宏。

运行期切换（按键/菜单）可以在 `AppController` 里加一个 `IVehicleSource*` 切换函数，
但 LVGL + BLE 同时驻留会显著增加 RAM 占用，需要先做内存评估。

## 6. 后端库使用边界

- PlatformIO 包目录：仓库根目录下的 `TeslaBleBackend/`，与 `TESLA-DASHBOARD/` 并列。
- 公共入口：`#include <TeslaBleBackend.h>`；`TeslaBleBackend` 提供 `begin()`、`loop()`、
  `state()`、`status()`。
- VIN、密钥和可覆盖参数通过项目 `include/tesla_backend_user_config.h` 配置；
  本仪表通过该文件转引自己的 `config.h`。
- 该库尚未拆成独立 Git 仓库，其他工程可复制库目录或将其父目录加入 `lib_extra_dirs`；
  发布/直接 Git 依赖需后续独立打包。

## 7. 回归检查清单

- [ ] `pio run` 通过，Flash/RAM 无明显增长（当前约 Flash 31.6% / RAM 28.6%）
- [ ] 演示模式（`CFG_DEMO_MODE=1`）能显示 9 阶段剧本
- [ ] BLE 模式能扫描、握手、显示仪表页
- [ ] 断连遮罩、配钥卡片、告警条行为与改造前一致
- [ ] 换板后检查：白屏（引脚/片选）、花屏（面板尺寸/offset）、红蓝互换（rgb_order）、反色（invert）
