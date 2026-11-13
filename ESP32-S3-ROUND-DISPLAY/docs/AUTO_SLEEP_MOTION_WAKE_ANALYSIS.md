# ESP32-S3 圆屏「静止自动休眠 / 运动唤醒」可行性分析

> 文档性质：分析 + 实现设计稿，**未修改任何代码**。  
> 目标读者：后续接手实现的 AI / 开发者。  
> 目标硬件：Waveshare **ESP32-S3-Touch-AMOLED-1.75-B**（SKU 31262），工程目录 `ESP32-S3-ROUND-DISPLAY/`。

> **已确认的设计决策（2026-11-13）：**
> - “运动”只指**设备/板子自身被移动**，由 QMI8658 IMU 判断；**不使用车辆 BLE 速度**作为休眠/唤醒依据。
> - “关机”采用 **ESP32-S3 deep sleep** 方案，接受“屏幕关闭、BLE 停止、可被运动唤醒”的形态；不使用 AXP2101 彻底断电。

---

## 0. 结论摘要（TL;DR）

**可以实现，但“关机”应定义为 ESP32-S3 深度睡眠（deep sleep），而不是 AXP2101 彻底断电。**

- 板载 **QMI8658 六轴 IMU**，可以感知板子/设备的运动。
- QMI8658 的 **INT2 直接接到 ESP32-S3 的 GPIO21**，GPIO21 是 RTC GPIO，可以作为 `ext1` 深度睡眠唤醒源。
- QMI8658 支持 **Wake on Motion（WoM）**：静止时进入低功耗，检测到运动后在 INT2 上产生中断，正好用于“运动唤醒”。
- 因此推荐方案：
  1. 系统运行时轮询 QMI8658，维护“最后运动时间”；
  2. 静止超过可配置时间（默认 60 秒）后，关闭屏幕、进入 ESP32 deep sleep；
  3. 睡眠前把 QMI8658 配成 WoM，GPIO21 配置为唤醒源；
  4. 有运动时 QMI8658 拉高/翻转 INT2，ESP32 被唤醒并重新启动固件。
- **不能用 AXP2101 彻底断电来实现“运动唤醒”**：AXP2101 关机后除 RTC LDO 外所有电源轨关闭，QMI8658 也断电，无法再感知运动；AXP2101 关机后只能由 PWR 键 / VBUS 插入等有限方式重新上电。
- 需要特别设计的一个体验问题：**在 deep sleep 下，板载 PWR 键不能直接唤醒 ESP32**（PWR 键只接 AXP2101 / TCA9554，不在 ESP32 RTC GPIO 上）。可以用 BOOT 键（GPIO0）或运动作为唤醒源；如果必须保留 PWR 键唤醒，需要额外硬件/电源方案，详见第 3、6 节。

---

## 1. 当前工程现状

### 1.1 已实现的内容

- `ESP32-S3-ROUND-DISPLAY/src/main.cpp`
  - `setup()`：串口初始化，调用 `DashboardApp::begin()`。
  - `loop()`：调用 `DashboardApp::loop()`，每轮 `delay(5)`。
- `ESP32-S3-ROUND-DISPLAY/src/app/DashboardApp.cpp`
  - `begin()`：初始化显示、LVGL UI、BOOT 键、PWR 键、触摸。
  - `beginPowerButton()`：`Wire.begin(15, 14)`，通过 TCA9554（I2C `0x20`）的 EXIO4 读 PWR 键。
  - `pollPowerButton()`：PWR 键长按 3 秒调用 `ESP.restart()`。
  - `pollBootButton()`：BOOT 键（GPIO0）短按旋转画面。
  - `pollTouch()`：CST9217 触摸，上下滑动调亮度。
  - `loop()`：轮询按键/触摸、启动 BLE 数据源、每 50 ms 刷新 UI。
- `ESP32-S3-ROUND-DISPLAY/src/data/TeslaUiSource.cpp`
  - 把 `VehicleState` 映射为 `UiModel`，其中 `speedValid` / `speedKmh` 可用于仪表显示；**本方案的自动休眠/唤醒不使用它们**。
- `ESP32-S3-ROUND-DISPLAY/include/config.h`
  - 现有配置都是编译期宏，例如 `CFG_TESLA_VIN`、`CFG_TESLA_DRIVE_POLL_INTERVAL_MS`、`CFG_DEMO_MODE`。

### 1.2 当前没有的内容

- 没有任何 QMI8658 / IMU 驱动或初始化代码。
- 没有任何 AXP2101 / 电源管理代码。
- 没有 `display.sleep()` / 熄屏接口；`RoundDisplay::setBrightness()` 会把亮度钳制在 26–204，不能直接用 0 熄屏。
- 没有深度睡眠 / 唤醒原因处理。
- `TeslaBleBackend` 没有显式 `end()` / 反初始化接口（深度睡眠会直接切断射频，通常不需要手动 deinit）。

### 1.3 工程约束

来自 `ESP32-S3-ROUND-DISPLAY/AGENTS.md`：

- **不要自动运行 `pio run` / 编译**，由用户自行编译烧录。
- **尽量不要改 `platformio.ini`**；改动会触发 LVGL / Arduino_GFX / Arduino core 全量重编（约 10–17 分钟）。
- 调参优先改 `include/config.h`、`include/lv_conf.h`、`src/display/RoundDisplay.cpp`。

> 实现建议：自己写一个很小的 QMI8658 I2C 驱动，避免为了一颗 IMU 往 `platformio.ini` 加第三方库。QMI8658 只需要读写几个寄存器，代码量很小。

---

## 2. 硬件事实（与实现直接相关）

来源：Waveshare 官方硬件参考、QMI8658 数据手册、AXP2101 数据手册。

| 项目 | 事实 | 对方案的影响 |
| --- | --- | --- |
| MCU | ESP32-S3R8，16 MB Flash / 8 MB PSRAM | 支持 deep sleep、RTC GPIO、ext1 唤醒 |
| IMU | QMI8658，六轴加速度计 + 陀螺仪，I2C 地址 `0x6B` | 可做运动检测 / WoM |
| IMU INT1 | 经 TCA9554 的 P6 / EXIO6 转接 | **不能直接作为 ESP32 唤醒源**，只能轮询扩展器 |
| IMU INT2 | 直接接 **GPIO21** | 可作为 ESP32-S3 `ext1` 深度睡眠唤醒源 |
| 共用 I2C | SCL = GPIO14，SDA = GPIO15 | 与触摸 CST9217、TCA9554、AXP2101、RTC 共用，不能重复 `Wire.begin` |
| PMU | AXP2101，I2C 地址 `0x34` | 可软件关机、电池充电/遥测；但关机后 IMU 也断电 |
| PWR 键 | 走 AXP2101 电源通路；整形后的状态经 TCA9554 EXIO4 读取 | 不在 ESP32 GPIO 上，deep sleep 下不可直接作为 GPIO 唤醒源 |
| BOOT 键 | GPIO0，按下为低，外部上拉 | GPIO0 是 RTC GPIO，可作为 deep sleep 唤醒源 |
| 显示 | CO5300 QSPI AMOLED，466×466 | 无独立背光 PWM；熄屏要写面板 sleep / 亮度 0 |
| 触摸 | CST9217，I2C `0x5A`，INT = GPIO11，RST = GPIO40 | 触摸可视为“用户活动”，应重置静止计时 |
| 电池 | 单节 3.7 V 锂电池接口，AXP2101 管理 | 待机功耗直接决定续航 |
| RTC | PCF85063，I2C `0x51`，INT 经 TCA9554 EXIO3 | 可作为定时唤醒的备选，但需要额外接线/配置 |

### 2.1 QMI8658 关键能力

QMI8658 支持：

- 加速度计 / 陀螺仪数据读取；
- Any-Motion / No-Motion / Significant-Motion 检测；
- Tap / Pedometer；
- **Wake on Motion（WoM）**：专门用于“主机休眠、运动时唤醒主机”的低功耗模式。

WoM 的关键寄存器与流程（来自 QMI8658 数据手册 / SensorLib 实现）：

| 寄存器 | 作用 |
| --- | --- |
| `CTRL1`（`0x02`） | bit3 = INT1 推挽/高阻；bit4 = INT2 推挽/高阻；bit6 = 地址自增 |
| `CTRL2`（`0x03`） | bit[7:4] 加速度量程；bit[3:0] 加速度 ODR |
| `CTRL7`（`0x08`） | bit0 = 加速度计使能；bit1 = 陀螺仪使能 |
| `CTRL8`（`0x09`） | 中断路由 / 运动检测使能 |
| `CTRL9`（`0x0A`） | 命令寄存器，WoM 配置命令 = `0x08` |
| `CAL1_L`（`0x0B`） | WoM 阈值，单位 1 mg；`0x00` 表示关闭 WoM |
| `CAL1_H`（`0x0C`） | bit[7:6] 中断引脚/初始电平；bit[5:0] blanking 样本数 |
| `STATUS1`（`0x2F`） | bit2 = WoM 事件；读 STATUS1 会清除 WoM 并复位中断线 |

WoM 配置顺序（概念上）：

1. 关闭传感器（`CTRL7 = 0x00`）；
2. 设置加速度计量程与 ODR（`CTRL2`，低功耗 ODR 可选 128/21/11/3 Hz）；
3. 写 WoM 阈值到 `CAL1_L`；
4. 写 `CAL1_H`：选择 INT2、初始电平、blanking 样本数；
5. 向 `CTRL9` 写 `0x08`（`CTRL_CMD_WRITE_WOM_SETTING`）；
6. 使能加速度计（`CTRL7` bit0）；
7. 使能 INT2 推挽输出（`CTRL1` bit4）。

> WoM 模式下**没有传感器数据输出**，只负责运动事件中断。唤醒后需要重新初始化 QMI8658 为普通加速度计模式。

### 2.2 AXP2101 关键能力

- 写 `REG10H[0] = 1` 可让 AXP2101 关机；关机后除 RTC LDO 外所有电源轨关闭，典型待机约 **40 µA**（数据手册）。
- 关机后的开机源：
  - PWR 键长按（ONLEVEL 可配 128 ms / 512 ms / 1 s / 2 s）；
  - VBUS 插入；
  - VBAT 从低到高；
  - IRQ 引脚低电平超过 16 ms（需要配置）；
  - 电池充到正常电压。
- AXP2101 还有“sleep / wakeup”模式（`REG26H`），可以关闭部分电源轨并保留部分电源，通过 IRQ 或软件唤醒；但本板 **QMI8658 的 INT2 没有接到 AXP2101 IRQ**，所以无法用“IMU 运动 → AXP IRQ → 整机唤醒”这条链路。
- 本工程当前**完全没有初始化 AXP2101**，所以 PWR 键行为、充电参数、关机阈值都还是芯片默认值。

---

## 3. 方案对比

### 方案 A：AXP2101 彻底关机（真关机）

做法：静止 60 秒后写 AXP2101 `REG10H[0] = 1`，整机断电；用户按 PWR 键重新上电。

| 维度 | 结果 |
| --- | --- |
| 静止自动关机 | ✅ 可以 |
| 运动自动开机 | ❌ **不可以**（IMU 已断电） |
| 待机功耗 | ✅ 最低，约 40 µA + 电池自放电 |
| 唤醒方式 | PWR 键 / VBUS 插入 / 部分配置下的 IRQ |
| 唤醒速度 | 冷启动，较慢（显示、BLE 重新初始化） |
| 代码复杂度 | 中：需要 AXP2101 驱动、关机前保存状态 |
| 硬件改动 | 不需要，但功能受限 |

**结论：适合“手动关机 / 长期存放”，不适合“运动唤醒”。**

### 方案 B：ESP32-S3 deep sleep + QMI8658 WoM（推荐）

做法：静止 60 秒后熄屏，配置 QMI8658 WoM，ESP32 进入 deep sleep；QMI8658 检测到运动后通过 INT2/GPIO21 唤醒 ESP32。

| 维度 | 结果 |
| --- | --- |
| 静止自动“关机” | ✅ 可以（系统休眠，屏幕/BLE/UI 全停） |
| 运动自动开机 | ✅ 可以 |
| 待机功耗 | 取决于板级外围，需实测；ESP32-S3 芯片 deep sleep 约 µA 级，QMI WoM + AXP 静态电流会叠加 |
| 唤醒方式 | QMI INT2（GPIO21）、BOOT 键（GPIO0）、可选 RTC 定时器 |
| 唤醒速度 | 比冷启动快，但仍会重新跑 `setup()`，需要重新初始化显示/BLE |
| 代码复杂度 | 中：IMU 驱动 + 电源状态机 + 熄屏 + 唤醒原因处理 |
| 硬件改动 | 不需要 |

**结论：满足“静止自动关机 + 运动自动开机”的目标，是当前硬件下的正确方案。**

### 方案 C：AXP2101 sleep + 保留 IMU 电源

理论上可以让 AXP2101 进入 sleep，只关闭 ESP32 的电源轨、保留 IMU 的电源轨，再把 IMU 中断接到 AXP2101 IRQ 实现唤醒。

但本板：

- QMI8658 INT2 只接到 ESP32 GPIO21；
- QMI8658 INT1 只接到 TCA9554 EXIO6；
- AXP2101 IRQ 只接到 TCA9554 EXIO5；
- 三者没有直接相连。

因此**不改硬件无法实现**。即使改线，还要保证 IMU 电源轨在 AXP sleep 时保持供电，复杂度高，不推荐。

### 方案 D：硬件改造实现“真关机 + 运动唤醒”

需要：

1. 把 QMI8658 INT2 引到 AXP2101 的 IRQ 引脚（或加逻辑门）；
2. 让 AXP2101 关机时仍给 QMI8658 供电（单独 LDO / 电池直供）；
3. 配置 AXP2101 的 IRQ 低电平开机；
4. 处理电平匹配、漏电、误唤醒。

**结论：不建议。除非产品化且对 40 µA 待机有硬性要求。**

### 方案对比表

| 需求 / 指标 | AXP 彻底关机 | ESP32 deep sleep + WoM |
| --- | --- | --- |
| 静止 60 s 自动关闭 | ✅ | ✅ |
| 运动自动唤醒 | ❌ | ✅ |
| 屏幕关闭 | ✅ | ✅ |
| BLE 关闭 | ✅ | ✅ |
| PWR 键唤醒 | ✅ | ⚠️ 不能直接（需额外方案） |
| BOOT 键唤醒 | ❌（整机断电） | ✅（GPIO0） |
| 待机功耗 | 约 40 µA | 约几十到一百多 µA，需实测 |
| 唤醒后 BLE 重连 | 冷启动 | 重新初始化，仍需重连 |
| 代码改动量 | 中 | 中 |
| 硬件改动 | 无 | 无 |

---

## 4. 推荐架构

### 4.1 名词定义

- **活动（Active）**：屏幕亮、UI 刷新、BLE 工作。
- **静止（Idle）**：连续 `CFG_AUTO_SLEEP_IDLE_MS` 没有检测到运动/用户操作/车辆运动。
- **休眠（Sleep）**：ESP32 deep sleep，屏幕关闭，BLE 停止，QMI8658 进入 WoM。
- **唤醒（Wake）**：QMI8658 检测到运动（或 BOOT 键/定时器），ESP32 重新启动。

### 4.2 状态机

```text
                 ┌─────────────────────────────┐
                 │           Active            │
                 │  屏幕亮 / UI / BLE 工作      │
                 └──────────────┬──────────────┘
                                │ 连续 idle >= 阈值
                                │ 且不处于配对/启动动画/
                                │ 充电禁止休眠等状态
                                ▼
                 ┌─────────────────────────────┐
                 │        Enter Sleep          │
                 │ 熄屏 → 配置 WoM → deep sleep │
                 └──────────────┬──────────────┘
                                │ QMI INT2 / BOOT / Timer
                                ▼
                 ┌─────────────────────────────┐
                 │          Wake Up            │
                 │ 读取唤醒原因 → 重新初始化     │
                 └──────────────┬──────────────┘
                                │ 确认确实有运动
                                ▼
                          回到 Active
```

### 4.3 什么算“运动”？

**本方案已确认：只使用 QMI8658 检测设备自身的物理移动。**

- 适用于设备被拿起、翻转、移动、放在车里随车身振动等场景。
- 不依赖 BLE，也不使用 `UiModel.speedKmh` / `speedValid`。
- 车辆速度数据可以保留给仪表显示，但**不参与**自动休眠/唤醒判断，避免把“车在动”和“设备在动”混为一谈。

**判定策略：**

- QMI8658 检测到超过阈值的加速度变化，且连续 N 个采样确认后，重置 `lastMotionMs`；
- 触摸按下、PWR 键操作、BOOT 键操作也应重置计时（用户正在使用）；
- 只有设备持续静止达到 `CFG_AUTO_SLEEP_IDLE_MS` 才休眠；
- 设备被移动时，QMI8658 WoM 通过 INT2/GPIO21 唤醒 ESP32。

> 注意：如果设备固定在车里，“设备移动”通常等价于车身振动/车辆行驶；但本方案不读取车速，因此车辆非常平稳地移动时是否触发，完全取决于 IMU 阈值和安装方式，需要实测调参。

### 4.4 休眠前必须检查的条件

满足以下全部条件才允许休眠：

- `CFG_AUTO_SLEEP_ENABLE == 1`；
- 已过启动动画（`dashboard.bootAnimationRunning() == false`）；
- 不在配钥流程中（`UiPairingPhase::Idle`）；
- 当前没有触摸按下 / 滑动；
- 距离最后一次“运动/操作”超过 `CFG_AUTO_SLEEP_IDLE_MS`；
- 如果 `CFG_AUTO_SLEEP_WHILE_CHARGING == 0`，则充电中不休眠；
- 可选：电量高于某个阈值（避免低电休眠后无法唤醒/损坏电池）。

> 不建议把“BLE 正在扫描/连接”作为永久阻止休眠的条件，否则车辆不在附近时设备会一直耗电。可以只在“配钥”阶段阻止休眠。

### 4.5 唤醒后的处理

ESP32 从 deep sleep 唤醒后，`setup()` 会重新执行，效果类似一次软重启：

- 读取 `esp_sleep_get_wakeup_cause()`；
- 如果是 `ESP_SLEEP_WAKEUP_EXT1`，说明是 GPIO21（QMI INT2）或 GPIO0（BOOT）唤醒；
- 读取 QMI8658 `STATUS1` 清除 WoM 事件，然后重新初始化 QMI8658 为普通加速度计模式；
- 正常初始化显示、LVGL、触摸、BLE；
- 可选：如果是运动唤醒，跳过开机动画，直接显示仪表，减少唤醒时间；
- 可选：唤醒后做 1–2 秒的“确认运动”，如果其实没有运动，重新进入休眠，避免误唤醒后一直亮屏。

> 注意：deep sleep 会丢失普通 RAM 状态，但 RTC 内存（`RTC_DATA_ATTR`）和 NVS 会保留。亮度/旋转已经存在 NVS 中，不需要额外处理。

---

## 5. 参数配置建议

### 5.1 编译期配置（推荐先做）

在 `include/config.h` 中增加以下宏（名称可按项目风格调整）：

```cpp
// ---------------------------------------------------------------------------
// 自动休眠 / 运动唤醒
// ---------------------------------------------------------------------------
#define CFG_AUTO_SLEEP_ENABLE 1

// 静止多久后进入 deep sleep；默认 60 秒
#define CFG_AUTO_SLEEP_IDLE_MS (60UL * 1000UL)

// 是否使用 QMI8658 检测板子运动
#define CFG_AUTO_SLEEP_IMU_ENABLE 1

// IMU 轮询间隔（运行时）；25~50 Hz 足够
#define CFG_AUTO_SLEEP_IMU_POLL_MS 40

// 运动判定阈值（mg，1 mg = 0.001 g）；建议先 40~80，再按实测调整
#define CFG_AUTO_SLEEP_MOTION_THRESHOLD_MG 50

// 连续多少个采样超过阈值才认为“真的在动”
#define CFG_AUTO_SLEEP_MOTION_CONFIRM_SAMPLES 3

// 本方案只使用设备自身运动（QMI8658），不读取车辆速度。
// 如果以后想加“车辆速度辅助判断”，再单独增加宏，不要默认启用。

// 充电中是否允许休眠
#define CFG_AUTO_SLEEP_WHILE_CHARGING 0

// 睡眠唤醒引脚：QMI8658 INT2 = GPIO21
#define CFG_AUTO_SLEEP_WAKE_GPIO 21

// WoM 阈值（mg）；越大越不敏感，越省电但越容易漏掉轻微运动
#define CFG_AUTO_SLEEP_WOM_THRESHOLD_MG 50

// WoM blanking 样本数，避免刚开启 WoM 时的瞬态误触发
#define CFG_AUTO_SLEEP_WOM_BLANKING 8

// 唤醒后确认运动的时间；0 = 不确认
#define CFG_AUTO_SLEEP_WAKE_CONFIRM_MS 2000

// 可选：定时唤醒兜底（0 = 关闭）；单位 ms
#define CFG_AUTO_SLEEP_TIMER_WAKE_MS 0
```

### 5.2 运行期配置（可选，后续做）

如果希望“不改固件就能调 1 分钟”，可以：

- 在 NVS 中新建 namespace `power`，key `idle_ms`；
- 启动时读取，缺省用 `CFG_AUTO_SLEEP_IDLE_MS`；
- 通过串口命令或设置页写入。

但当前工程没有设置页/串口命令框架，**第一阶段建议先用编译期宏**，稳定后再加 NVS。

### 5.3 参数调优建议

- `CFG_AUTO_SLEEP_IDLE_MS`：60 秒是合理起点；如果希望更省电可改 30 秒，但频繁休眠/唤醒体验会变差。
- `CFG_AUTO_SLEEP_MOTION_THRESHOLD_MG`：
  - 50 mg 约等于 0.05 g，能感知明显拿起/车身振动；
  - 太小会被桌面轻微振动/风扇误触发；
  - 太大可能漏掉“车开始缓慢移动”。
- `CFG_AUTO_SLEEP_WOM_THRESHOLD_MG`：
  - 可以和运行时阈值相同或略高；
  - 建议先 50–100 mg 实测。
- `CFG_AUTO_SLEEP_WOM_BLANKING`：
  - 8–32 个样本；ODR 越低，blanking 时间越长。
- `CFG_AUTO_SLEEP_WHILE_CHARGING`：
  - 如果设备长期插 USB 当桌面仪表，设为 0（充电不休眠）；
  - 如果希望充电也休眠省电，设为 1。

---

## 6. 实现设计（给后续 AI 的落地指引）

### 6.1 新增文件建议

```text
ESP32-S3-ROUND-DISPLAY/
├── include/
│   ├── input/
│   │   └── Qmi8658.h          # QMI8658 最小驱动
│   └── power/
│       └── PowerManager.h     # 休眠状态机
└── src/
    ├── input/
    │   └── Qmi8658.cpp
    └── power/
        └── PowerManager.cpp
```

不新增第三方库，避免改 `platformio.ini`。

### 6.2 `Qmi8658` 驱动职责

建议提供最小接口：

```cpp
class Qmi8658 {
 public:
  bool begin(TwoWire& wire, uint8_t address = 0x6B);
  bool readAccel(float& x, float& y, float& z);  // 单位 g
  bool configureWakeOnMotion(uint8_t thresholdMg, uint8_t blankingSamples);
  bool clearWakeStatus();                        // 读 STATUS1
  bool isPresent() const;
};
```

实现要点：

1. **复用现有 `Wire`**：`DashboardApp::beginPowerButton()` 已经 `Wire.begin(15, 14)`，QMI 初始化必须在这之后，不要再次 `Wire.begin`。
2. I2C 地址 `0x6B`；读 `WHO_AM_I` 确认芯片存在（可选）。
3. `readAccel()`：
   - 读 `AX_L` 开始的 6 字节；
   - 小端组合为 `int16_t`；
   - 按量程换算（例如 ±8 g：`raw * 8.0 / 32768.0`）。
4. `configureWakeOnMotion()`：
   - 参考第 2.1 节流程；
   - 选择 **INT2、初始电平 0、推挽输出**；
   - 阈值单位 mg；
   - 写 `CTRL9 = 0x08`；
   - 使能加速度计和 INT2。
5. `clearWakeStatus()`：
   - 读 `STATUS1`（`0x2F`），清除 WoM 位并复位中断线。
6. 建议所有 I2C 操作加超时/错误返回，避免总线异常时卡死主循环。
7. 读写频率：运行时 25–50 Hz 足够；不要在 `loop()` 里无节制地每 5 ms 读一次。

### 6.3 `PowerManager` 职责

建议状态与接口：

```cpp
class PowerManager {
 public:
  void begin(Qmi8658* imu, RoundDisplay* display);
  void notifyUserActivity();                 // 触摸/按键
  void update(const UiModel& model,
              bool bootAnimationRunning,
              bool charging);
  bool wokeByMotion() const;                 // 读取唤醒原因
  void confirmWakeOrSleepAgain();            // 可选：唤醒后确认运动
 private:
  void enterDeepSleep();
  bool shouldSleep(...) const;
};
```

内部状态：

- `uint32_t m_lastMotionMs`：最后一次设备运动/用户操作时间；
- `uint32_t m_lastImuPollMs`：IMU 轮询节流；
- `bool m_imuAvailable`；
- `bool m_charging`；
- `bool m_wokeByExt1`（可用 `RTC_DATA_ATTR` 保存）。

`update()` 逻辑（伪代码）：

```cpp
void PowerManager::update(const UiModel& model,
                          bool bootAnimationRunning,
                          bool charging) {
  const uint32_t now = millis();

  // 1. IMU 运动检测（节流）
  //    只判断设备自身是否被移动，不读取 model.speedKmh。
  if (CFG_AUTO_SLEEP_IMU_ENABLE && m_imuAvailable &&
      now - m_lastImuPollMs >= CFG_AUTO_SLEEP_IMU_POLL_MS) {
    m_lastImuPollMs = now;
    float x, y, z;
    if (m_imu->readAccel(x, y, z)) {
      const float mag = sqrtf(x * x + y * y + z * z);
      // 静止时 mag 约等于 1g；这里用偏离 1g 的幅度判断
      const float deltaMg = fabsf(mag - 1.0f) * 1000.0f;
      if (deltaMg >= CFG_AUTO_SLEEP_MOTION_THRESHOLD_MG) {
        if (++m_motionSamples >= CFG_AUTO_SLEEP_MOTION_CONFIRM_SAMPLES) {
          m_lastMotionMs = now;
          m_motionSamples = 0;
        }
      } else {
        m_motionSamples = 0;
      }
    }
  }

  // 2. 是否允许休眠
  if (!CFG_AUTO_SLEEP_ENABLE) return;
  if (bootAnimationRunning) return;
  if (model.pairing != UiPairingPhase::Idle) return;
  if (CFG_AUTO_SLEEP_WHILE_CHARGING == 0 && charging) return;

  // 3. 设备持续静止达到阈值 -> 休眠
  if (now - m_lastMotionMs >= CFG_AUTO_SLEEP_IDLE_MS) {
    enterDeepSleep();
  }
}
```

> 上面的“偏离 1g”是最简单实现。更稳的做法是维护一个慢速重力估计 `gravityLPF`，用 `|acc - gravityLPF|` 判断线性加速度/姿态变化；这样对“缓慢翻转”和“持续振动”都更可靠。第一版可以先用简单版，实测后再优化。

### 6.4 进入 deep sleep 的步骤

```cpp
void PowerManager::enterDeepSleep() {
  // 1. 防止重复进入
  if (m_enteringSleep) return;
  m_enteringSleep = true;

  // 2. 关闭 UI 刷新 / 熄屏
  m_display->setBrightnessRaw(0);   // 需要新增接口，绕过 26~204 钳制
  m_display->panelSleep(true);      // 需要新增 CO5300 sleep-in 命令

  // 3. 配置 QMI8658 WoM
  m_imu->configureWakeOnMotion(CFG_AUTO_SLEEP_WOM_THRESHOLD_MG,
                               CFG_AUTO_SLEEP_WOM_BLANKING);

  // 4. 配置唤醒源
  esp_sleep_enable_ext1_wakeup(1ULL << CFG_AUTO_SLEEP_WAKE_GPIO,
                               ESP_EXT1_WAKEUP_ANY_HIGH);

  // 可选：BOOT 键也作为唤醒源（GPIO0，按下为低）
  // 注意：ext1 多个引脚时无法直接区分，需要唤醒后读电平
  // esp_sleep_enable_ext1_wakeup((1ULL << 21) | (1ULL << 0),
  //                              ESP_EXT1_WAKEUP_ANY_HIGH); // GPIO0 是低有效，需单独处理

  // 可选：定时唤醒兜底
#if CFG_AUTO_SLEEP_TIMER_WAKE_MS > 0
  esp_sleep_enable_timer_wakeup((uint64_t)CFG_AUTO_SLEEP_TIMER_WAKE_MS * 1000ULL);
#endif

  Serial.flush();
  esp_deep_sleep_start();  // 不返回
}
```

注意：

- `esp_sleep_enable_ext1_wakeup()` 的位图只支持 RTC GPIO；**GPIO21 在 ESP32-S3 的 RTC GPIO 范围（0–21）内**。
- 如果同时使用 GPIO21（高有效）和 GPIO0（低有效），不能简单用同一个 `ANY_HIGH`；需要分别处理，或只用 GPIO21 作为运动唤醒、BOOT 键用另一个唤醒源/不唤醒。
- QMI8658 WoM 的 INT2 默认电平建议设为 0，运动时翻转为 1，因此用 `ESP_EXT1_WAKEUP_ANY_HIGH` 匹配。
- 如果原理图上 INT2 有外部上拉/下拉，需要确认空闲电平，避免休眠时误唤醒。
- `RoundDisplay` 目前没有 `setBrightnessRaw()` / `panelSleep()`，需要新增；不要直接改 `setBrightness()` 的钳制逻辑，以免影响滑动手势。

### 6.5 唤醒后的处理

```cpp
void PowerManager::begin(Qmi8658* imu, RoundDisplay* display) {
  m_imu = imu;
  m_display = display;

  const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  m_wokeByExt1 = (cause == ESP_SLEEP_WAKEUP_EXT1);

  if (m_wokeByExt1) {
    // 清除 QMI WoM 事件，恢复普通加速度计模式
    m_imu->clearWakeStatus();
    m_imu->begin(Wire, 0x6B);  // 重新初始化普通模式
  }

  m_lastMotionMs = millis();
  m_lastVehicleMotionMs = millis();
}
```

可选：如果 `CFG_AUTO_SLEEP_WAKE_CONFIRM_MS > 0`，在唤醒后读取几秒 IMU；如果没有真实运动，重新进入休眠，避免“被人碰一下就亮屏很久”。

### 6.6 与 `DashboardApp` 的集成点

建议改动：

1. `include/app/DashboardApp.h`
   - 增加 `Qmi8658 m_imu;`
   - 增加 `PowerManager m_power;`
   - 增加 `bool m_touchActivity` 或从 `pollTouch()` 返回“本次是否有按下”。

2. `src/app/DashboardApp.cpp`
   - `beginPowerButton()` 里 `Wire.begin(15, 14)` 之后初始化 `m_imu.begin(Wire, 0x6B)`；
   - `begin()` 末尾调用 `m_power.begin(&m_imu, &display)`；
   - `pollTouch()` / `pollBootButton()` / `pollPowerButton()` 检测到用户操作时调用 `m_power.notifyUserActivity()`；
   - `loop()` 中在 `m_model = m_source.model()` 之后调用：

     ```cpp
     m_power.update(m_model,
                    dashboard.bootAnimationRunning(),
                    m_model.charging);
     ```

   - 注意 `m_model.charging` 是车辆充电状态，不是“USB 是否插入”。如果要判断“正在充电/USB 供电”，需要读 AXP2101 的 VBUS/充电状态，或先用 `m_model.charging` 近似。

3. `include/display/RoundDisplay.h` / `src/display/RoundDisplay.cpp`
   - 增加 `setBrightnessRaw(uint8_t)`（不钳制）；
   - 增加 `panelSleep(bool)`，向 CO5300 写 sleep-in / sleep-out 命令；
   - 唤醒后恢复之前保存的亮度（NVS 已有 `bri`）。

4. `src/main.cpp`
   - 一般不需要改；`PowerManager` 在 `DashboardApp::begin()` 中读取唤醒原因即可。
   - 如果希望“运动唤醒时跳过开机动画”，可在 `DashboardApp::begin()` 或 `LvglDashboard` 中根据 `m_power.wokeByMotion()` 决定是否播放 boot 动画。

### 6.7 关于 AXP2101（可选扩展）

如果后续仍想提供“真正关机”入口（例如长按 PWR 或设置页“关机”），可以：

- 自己写 3 行 I2C 代码：读 `0x10`，置 bit0，写回；或使用 XPowersLib；
- 关机前保存需要保存的状态；
- 明确告知用户：关机后只能按 PWR 键 / 插 USB 开机，**运动不会开机**；
- 不要和当前“PWR 长按 3 秒重启”逻辑冲突：需要决定 PWR 长按到底是“重启”还是“关机”。

> 当前固件的 PWR 长按 3 秒重启，是通过 TCA9554 EXIO4 软件检测实现的。AXP2101 自己也有 OFFLEVEL（默认 4 秒）长按关机。两者可能产生竞争，改电源逻辑时要一起考虑。

---

## 7. 关键风险与边界情况

### 7.1 运动误判 / 漏判

- **误唤醒**：桌面振动、风扇、关门、路过车辆、雨滴、扬声器振动都可能触发。
  - 对策：提高 WoM 阈值、增加 blanking、唤醒后确认运动、必要时连续 N 次才唤醒。
- **漏唤醒**：车辆非常平稳地起步、设备固定很牢、阈值过高。
  - 对策：有 BLE 速度时优先用速度；WoM 阈值不要设太高；实测调整。
- **缓慢翻转**：单纯看 `|a| - 1g` 对“缓慢改变姿态”不敏感。
  - 对策：使用重力低通滤波 + 向量差，或使用 QMI 的 Any-Motion / Significant-Motion。

### 7.2 唤醒循环 / 睡不下去

- 如果 QMI INT2 一直为高（配置错误、外部上拉、WoM 初始电平不对），ESP32 可能刚睡就醒。
  - 对策：睡眠前读 GPIO21 电平；确认 WoM 初始电平；必要时加内部下拉/外部下拉；唤醒后清 `STATUS1` 并重新配置。
- 如果唤醒后马上又满足休眠条件，可能反复休眠/唤醒。
  - 对策：唤醒后至少保持 Active `CFG_AUTO_SLEEP_WAKE_CONFIRM_MS`；用 `RTC_DATA_ATTR` 记录短时间内的唤醒次数，异常时延长休眠或改用 AXP 关机。

### 7.3 PWR 键无法唤醒 deep sleep

这是当前硬件最需要提前说明的限制：

- PWR 键 → AXP2101 / TCA9554 EXIO4，不在 ESP32 GPIO 上；
- deep sleep 时 I2C 关闭，ESP32 无法读 TCA9554；
- 所以用户按 PWR 键时，ESP32 不会醒；AXP2101 可能按自己的长按逻辑关机/重启。

可选缓解：

1. **运动唤醒为主**：用户拿起设备即可唤醒；
2. **BOOT 键作为辅助唤醒**：GPIO0 是 RTC GPIO，可配置为唤醒源；
3. **定时唤醒**：例如每 5–30 分钟醒来一次，检查 BLE/电量，但会增加平均功耗；
4. **AXP 关机 + PWR 开机**：如果用户更习惯按键，提供“真关机”模式，但失去运动唤醒；
5. **硬件改线**：把 PWR 键状态接到某个 RTC GPIO（需要改板），不推荐。

### 7.4 充电 / USB 供电

- 如果设备长期插 USB，休眠会导致串口断开、看不到日志，调试时容易误以为死机。
- 建议：
  - 调试阶段可用 `CFG_AUTO_SLEEP_ENABLE 0` 或 `CFG_AUTO_SLEEP_WHILE_CHARGING 0`；
  - 正式使用再打开自动休眠；
  - 明确“充电中是否允许休眠”的产品策略。

### 7.5 BLE 重连与车辆状态

- 休眠会断开 BLE；唤醒后需要重新扫描/连接/握手，仪表可能先显示 Scanning/Syncing。
- 车辆本身的 BLE 可能已经休眠，唤醒后不一定立刻连上。
- 本方案不读取车速，所以“设备静止 60 秒”可能在车辆仍连接/仍显示时触发休眠；这是预期行为。
- 如果用户希望“车辆行驶时绝不休眠”，需要额外把车速作为阻止休眠的条件；当前已确认不做，后续如需再加。

### 7.6 显示与触摸功耗

- CST9217 触摸芯片在 3.3V 上，deep sleep 时仍可能耗电；如果实测待机电流偏高，可以给触摸芯片发 sleep 命令或通过 TCA9554 控制其复位/电源（需查原理图）。
- CO5300 AMOLED 熄屏要写 panel sleep-in 或亮度 0；只关 LVGL 刷新不够。

### 7.7 低电量

- 电池电压过低时进入 deep sleep 可能无法可靠唤醒；建议在休眠前检查 AXP2101 电池电压，低于阈值时保持关机/不反复唤醒。
- AXP2101 有 VOFF 阈值（默认约 2.6 V，可配），可防止过放。

---

## 8. 测试计划

### 8.1 功能测试

| 用例 | 预期 |
| --- | --- |
| 静止 60 秒 | 屏幕熄灭，串口输出休眠日志，电流下降 |
| 静止 30 秒时拿起设备 | 计时重置，不休眠 |
| 静止 60 秒后拿起设备 | QMI WoM 唤醒，ESP32 重启，UI 恢复 |
| 静止 60 秒后短按 BOOT | 如果配置了 GPIO0 唤醒，应唤醒 |
| 静止 60 秒后按 PWR | 按当前硬件，ESP32 不会醒；记录实际行为（AXP 是否关机/重启） |
| 设备静止但 BLE 仍在连接/显示 | 60 秒后仍应休眠（本方案不看车速） |
| 设备轻微移动（如轻碰桌面） | 根据阈值决定是否重置计时，需实测 |
| 配钥过程中 | 不休眠 |
| 充电中（`CFG_AUTO_SLEEP_WHILE_CHARGING=0`） | 不休眠 |
| 唤醒后无真实运动 | 确认窗口后重新休眠，不反复亮屏 |

### 8.2 功耗测试

用 USB 电流表 / 万用表 / 可调电源测量：

1. 正常运行（屏幕亮、BLE 扫描/连接）；
2. 熄屏但 ESP32 运行；
3. deep sleep + QMI WoM；
4. AXP2101 彻底关机（对比）。

记录：

- 平均电流；
- 唤醒瞬间峰值；
- 电池容量对应理论待机天数；
- 是否达到产品目标。

### 8.3 稳定性测试

- 连续 24–72 小时静置，确认不会自己醒/睡不下去；
- 反复拿起/放下/翻转 100 次，确认每次都能唤醒；
- 不同放置方式（桌面、支架、车内）和不同振动源下的误唤醒/漏唤醒测试；
- 低电量、USB 插拔、BLE 断连、配钥中断等异常路径。

---

## 9. 已确认决策与剩余问题

### 9.1 已确认

1. **“运动”指什么？**
   - ✅ **设备/板子自身被移动**，由 QMI8658 判断。
   - ❌ 不使用车辆 BLE 速度；车速只用于仪表显示，不参与休眠/唤醒。

2. **“关机”指什么？**
   - ✅ **熄屏 + ESP32 deep sleep**，接受运动唤醒。
   - ❌ 不使用 AXP2101 彻底断电。

3. **配置方式？**
   - ✅ **编译期 `config.h` 宏**（第一阶段）；运行期 NVS 配置以后再说。

### 9.2 仍需确认（实现前/实现时可决定）

1. **PWR 键是否必须能唤醒？**
   - 当前硬件 deep sleep 下不能直接支持；如果必须，需要重新评估电源方案或改硬件。

2. **充电/USB 供电时是否允许休眠？**
   - 建议默认不允许，避免调试时断开串口。

3. **唤醒体验？**
   - 唤醒后是否跳过开机动画？
   - 唤醒后如果 2 秒内没有真实运动，是否自动重新休眠？

4. **目标续航？**
   - 电池容量、期望待机天数，用于评估 deep sleep 的待机电流是否足够。

---

## 10. 参考链接

- Waveshare ESP32-S3-Touch-AMOLED-1.75 硬件参考：  
  <https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75/blob/main/HARDWARE_REFERENCE_ZH.md>
- Waveshare 产品页：  
  <https://www.waveshare.net/shop/ESP32-S3-Touch-AMOLED-1.75-B.htm>
- QMI8658C 数据手册：  
  <https://qstcorp.com/upload/pdf/202202/QMI8658C%20datasheet%20rev%200.9.pdf>
- QMI8658A 数据手册：  
  <https://files.waveshare.com/upload/5/5f/QMI8658A_Datasheet_Rev_A.pdf>
- AXP2101 数据手册：  
  <https://wiki.lilygo.cc/datasheet/AXP2101_Datasheet_V1.4_en.pdf>
- SensorLib QMI8658 驱动（含 WoM 示例）：  
  <https://github.com/lewisxhe/SensorLib>
- ESP-IDF 睡眠模式 API：  
  <https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/system/sleep_modes.html>

---

## 11. 一句话给后续实现者

**用 QMI8658 的 INT2/GPIO21 + ESP32-S3 deep sleep 实现“设备静止 60 秒休眠、设备移动唤醒”；运动只由 IMU 判断，不读取车辆速度。不要用 AXP2101 彻底断电来实现运动唤醒，因为关机后 IMU 也断电。PWR 键在 deep sleep 下不能直接唤醒，这是本板硬件限制，需要产品上接受或额外改线。**
