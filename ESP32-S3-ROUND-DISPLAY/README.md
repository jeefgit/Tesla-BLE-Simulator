# ESP32-S3 圆屏固件

面向微雪 Waveshare **ESP32-S3-Touch-AMOLED-1.75-B**（SKU 31262）的独立
PlatformIO 固件工程。本目录与现有 `TESLA-DASHBOARD` 分开，后续可复用仓库根目录的
`TeslaBleBackend` 作为车辆 BLE 数据源。

硬件参数依据微雪[产品页](https://www.waveshare.net/shop/ESP32-S3-Touch-AMOLED-1.75-B.htm)
及[官方硬件参考](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75/blob/main/HARDWARE_REFERENCE_ZH.md)。
`-B` 为带外壳版本；显示、触摸和主板连接与标准版相同。

| 项目 | 参数 |
| --- | --- |
| MCU | ESP32-S3R8 |
| Flash / PSRAM | 16 MB Flash / 8 MB OPI PSRAM |
| 圆形 AMOLED | 1.75 英寸，466 × 466，CO5300，QSPI |
| LCD 引脚 | SIO0–3: GPIO 4–7；PCLK: GPIO 38；CS: GPIO 12；RESET: GPIO 39 |
| 触摸 | CST9217 电容触摸，I²C 地址 `0x5A` |
| 触摸引脚 | SDA: GPIO 15；SCL: GPIO 14；INT: GPIO 11；RESET: GPIO 40 |
| 按键 | BOOT: GPIO0（按下为低）；PWR: 由 AXP2101 供电通路控制，经 TCA9554 EXIO4 读取 |
| IMU | QMI8658 六轴，I²C 地址 `0x6B`；INT2 直连 GPIO21（可作 deep sleep 唤醒源） |

AMOLED 亮度由 CO5300 面板命令控制，没有独立背光 PWM 引脚；固件支持在屏幕上
上下滑动实时调节亮度（见“上下滑动调节亮度”一节）。
板载 I²C 是共享总线；不要另建占用 GPIO14/15 的独立 I²C 总线。

## 目录结构

```text
ESP32-S3-ROUND-DISPLAY/
├── platformio.ini
├── design/
│   ├── UI-DESIGN.md            # 圆屏界面设计说明（坐标/配色/状态/数据映射）
│   ├── dashboard-ui.html       # 1:1 可交互设计稿（浏览器打开）
│   └── preview-*.png           # 四个状态的预览图
├── include/
│   ├── config.h
│   ├── app/
│   │   └── DashboardApp.h
│   ├── audio/
│   │   └── SoundSensor.h        # 声音传感器
│   ├── core/
│   │   └── UiModel.h
│   ├── data/
│   │   ├── IUiModelSource.h
│   │   └── TeslaUiSource.h      # VehicleState / SourceStatus → UiModel
│   ├── display/
│   │   ├── IDisplay.h
│   │   ├── RoundDisplay.h
│   │   └── ScreenShot.h        # 双击截图：LVGL 快照 → BMP → 串口 base64
│   ├── input/
│   │   ├── Cst9217Touch.h       # CST9217 触摸轮询驱动
│   │   └── Qmi8658.h            # QMI8658 IMU 最小驱动（加速度 / WoM）
│   ├── log/
│   │   └── EventLog.h           # 设备事件日志：RAM 缓冲 + LittleFS 槽轮转
│   ├── ota/
│   │   └── UsbOta.h              # USB CDC 网页 OTA：写备用分区 + 进度页
│   ├── platform/
│   │   ├── BoardProfile.h
│   │   └── boards/
│   │       └── waveshare_esp32_s3_touch_amoled_175_b.h
│   ├── power/
│   │   └── PowerManager.h       # 静止自动休眠 / 运动唤醒状态机
│   └── ui/
│       ├── ElfOverlay.h
│       ├── IDashboard.h
│       └── LvglDashboard.h
├── src/
│   ├── app/
│   │   └── DashboardApp.cpp
│   ├── audio/
│   │   └── SoundSensor.cpp
│   ├── data/
│   │   └── TeslaUiSource.cpp
│   ├── display/
│   │   ├── RoundDisplay.cpp
│   │   └── ScreenShot.cpp
│   ├── input/
│   │   ├── Cst9217Touch.cpp
│   │   └── Qmi8658.cpp
│   ├── log/
│   │   └── EventLog.cpp         # 挂载 / 轮转 / meta / LOG_* 串口命令
│   ├── ota/
│   │   └── UsbOta.cpp             # 串口协议 + Update.h 写 OTA 分区
│   ├── power/
│   │   └── PowerManager.cpp
│   ├── ui/
│   │   ├── ElfOverlay.cpp
│   │   ├── LvglDashboard.cpp
│   │   └── TeslaBootLogo.cpp
│   └── main.cpp
└── tools/
    ├── decode_screenshot.py       # 从串口日志还原 .bmp
    └── ota/                       # 本地网页 OTA（index.html + serve.py）
```

## 界面设计

- `design/UI-DESIGN.md`：466×466 圆屏布局、安全区、配色/字号 token、状态机与数据映射
- `design/dashboard-ui.html`：浏览器打开的 1:1 设计稿（可勾选安全圆/中心线）
- `design/preview-*.png`：行驶、充电、断连扫描、配钥四个状态预览

设计稿沿用 `TESLA-DASHBOARD` 的黑底 / Tesla 红 / 绿色能量回收语言，但按圆屏重排：
速度环（R170，204°–336°）包围车速大字，顶部收窄状态带，底部只保留两张信息卡。
驾驶 / 充电共用同一套骨架（速度环 ↔ SOC 环），未连接和配钥使用整屏状态页。

## 界面实现

固件已通过 Arduino_GFX 初始化 CO5300 QSPI 面板，并用 LVGL 9 实现设计稿中的
行驶、充电、断连/扫描和配钥页面。实时数据由 `TeslaBleBackend` 提供：
`src/data/TeslaUiSource.cpp` 把 `VehicleState` / `SourceStatus` 映射成 `UiModel`，
`DashboardApp` 每轮主循环刷新一次。`include/config.h` 中的 `CFG_DEMO_MODE`
设为 `1` 可回到静态演示数据。CST9217 触摸已接入，用于上下滑动调节亮度（见下文）。

### 按键与画面旋转

板载 **PWR 键长按 3 秒** 会重启固件。PWR 不在 ESP32 的 GPIO 上，而是经
TCA9554（I2C `0x20`）的 EXIO4 读取（高电平 = 按下）：`DashboardApp` 启动时
把 EXIO4 配置为输入，主循环中 30 ms 消抖检测，按住满 3 秒调用
`ESP.restart()`。开机时若 PWR 仍被按住（例如正用它上电），会先等松开再进入
长按检测，避免上电动作被误判成重启。

短按板载 **BOOT 键（GPIO0，按下为低）** 可把画面顺时针旋转 90°：
0° → 90° → 180° → 270° → 0°，如此循环。实现分两层：

- `DashboardApp::pollBootButton()`：30 ms 消抖，检测到有效按下后调用
  `RoundDisplay::rotateClockwise()`；按住不放只触发一次。
- `RoundDisplay`：CO5300 的 MADCTL 只能做镜像、不能交换 XY，因此旋转在
  flush 回调里用软件完成——RGB565 像素块重排到一块 466×50 的临时缓冲
  （优先 PSRAM），再按旋转后的窗口坐标写给面板；切换时整屏标脏重绘。

旋转方向会保存到 NVS（namespace `display`，key `rot`）：每次按键后立即写入，
`RoundDisplay::begin()` 启动时读回，因此重启、断电或正常重新烧录固件后都会
保持上次的方向，开机第一帧即为该方向。只有全片擦除（`pio run -t erase` /
`esptool erase_flash`）、烧录包含 NVS 分区的整片镜像，或更换分区表时才会
恢复默认 0°；想手动恢复默认方向，按 BOOT 键转回 0° 覆盖保存即可。

板载只有 BOOT 和 PWR 两个按键，没有独立 RESET 键；复位由 `CHIP_PU` 控制，
程序无法把复位动作当成普通按键读取，所以旋转触发使用 BOOT 键，重启使用
PWR 长按。

### 上下滑动调节亮度

单指在屏幕上**上滑调亮、下滑调暗**，亮度实时跟手变化，松手后保存。触摸只用于
这个手势，**没有注册 LVGL indev**，因此不会与界面控件抢事件。

手势判定（坐标均为 LVGL 画面坐标，已按当前旋转换算）：

- 按住后纵向累计位移 `|dy| ≥ 24 px`，且 `|dy| > |dx| + 16 px` 才激活；
- 一旦判定为横向拖动，本次按下不再触发亮度手势；
- 激活后 `1 px ≈ 1 档`亮度，以激活点为锚点避免起始跳动；范围限制在
  **10%–80%**（26–204）之间，防止过亮或过暗。

三个阈值和灵敏度都在 `src/app/DashboardApp.cpp` 顶部：

```cpp
constexpr int32_t kSwipeActivatePx = 24;      // 激活所需纵向位移
constexpr int32_t kSwipeDominancePx = 16;     // 纵向需领先横向的幅度
constexpr int32_t kBrightnessStepPerPx = 1;   // 每像素对应亮度档位
```

**旋转感知**：触摸芯片报告的是面板物理坐标，而画面在 flush 时被软件旋转了
`m_rotation` 个 90°。`RoundDisplay::mapTouchToScreen()` 对坐标做
`blitRotated()` 的逆变换（0°/90°/180°/270° 分别对应不同的坐标映射），使
"屏幕上的上下"在任何旋转角度下都指向同一个手势方向，与用户看到的画面一致。

**亮度控制**：写 CO5300 的 0x51 寄存器（`Arduino_CO5300::setBrightness()`，
0–255）。`RoundDisplay::setBrightness()` 会把亮度统一钳制在
`kMinBrightness`–`kMaxBrightness`（26–204，即 10%–80%）之间，从 NVS 读回的
旧值也会重新钳制。拖动过程中只写寄存器；松手时才由
`RoundDisplay::saveBrightness()` 写回 NVS（namespace `display`，key `bri`），
避免频繁擦写 flash。默认亮度 160，重启/断电后保持上次调节结果。

**触摸驱动**（`src/input/Cst9217Touch.cpp`）：按 Waveshare 自带的 SensorLib
`TouchDrvCST92xx` 流程实现，复用 PWR 键那条 I2C 总线（GPIO15/14，从地址
`0x5A`）：

1. 拉低触摸 RESET（GPIO40）10 ms 复位，等待固件启动；
2. 写 `0xD1 0x01` 进命令模式，依次读固件校验码 `0xD1FC`（高 16 位应为
   `0xCACA`）、分辨率 `0xD1F8`、芯片类型 `0xD204`（`0x9217`）；
3. 主循环每 30 ms 轮询一次：写 `0xD000` 请求 15 字节报告帧，再写
   `0xD0 0x00 0xAB` 应答；只解析第一点的坐标（`event == 0x06` 表示按下）。

写寄存器地址与读数据之间发 **STOP**（`endTransmission()`）而不是 repeated
start——CST9217 对后者不应答。

调试时可看串口（115200）日志：

```text
[TOUCH] firmware info: .. .. CA CA
[TOUCH] CST9217 ready (res 466x466)
```

手势日志（`[GESTURE] ...` / `[TOUCH] brightness ...`）默认已关闭，避免滑动时串口刷屏；
需要排查手势时可在 `src/app/DashboardApp.cpp` 的 `pollTouch()` 里临时加回 `CFG_LOG`。

### 双击截图（USB 串口）

双击屏幕（两次轻点，间隔 < 400 ms，位置相差 < 20 px）会把当前 LVGL 画面
渲染成 **466×466 的 24bit BMP**，按当前旋转方向输出，再以 base64 从 USB
串口打印。固件不保存文件、也不联网，图片需要 PC 端从串口日志里还原。

**取图步骤**

```bash
# 1. 打开串口监视器；log2file 会把原始日志写到 platformio-device-monitor-*.log
pio device monitor -f log2file -f default

# 2. 等开机完成后，在屏幕上连点两下
#    串口会先打印 ===SHOT-BEGIN===，随后是约 87 万字符的 base64，最后 ===SHOT-END===

# 3. Ctrl+C 退出，然后在工程目录运行解码脚本
python tools/decode_screenshot.py platformio-device-monitor-*.log
# 需要 PNG 时（依赖 Pillow）：加 --png
python tools/decode_screenshot.py shot.log --png
```

输出为 `shot-<时间戳>.bmp`，方向与屏幕当前方向一致（BOOT 键旋转 90° 后，
截图也会跟着转）。想直接管道也可以：

```bash
pio device monitor | python tools/decode_screenshot.py -
```

**实现说明**

- `include/display/ScreenShot.h` / `src/display/ScreenShot.cpp`：用 LVGL 9 的
  `lv_snapshot_take()` 把整屏渲染到 PSRAM（RGB565，约 434 KB），再转成 BMP
  （约 652 KB）；按 `RoundDisplay::rotation()` 做与 flush 相同的旋转变换。
- 串口输出期间会临时静音其他 `CFG_LOG` 日志（`cfglog::muted()`），并且每行
  base64 带 `SHOT:` 前缀，避免 BLE 回调日志把图片数据弄花。
- 截图时主循环会阻塞 1–5 秒（取决于 PC 读取串口的速度），画面暂时不刷新属正常。
- 内存峰值约 1.1 MB，全部在 PSRAM；抓完立即释放。
- 开关和阈值在 `include/config.h`：
  `CFG_SCREENSHOT_ENABLE` / `CFG_SCREENSHOT_DOUBLE_TAP_MS` / `CFG_SCREENSHOT_TAP_PX`。
- `LV_USE_SNAPSHOT` 已在 `include/lv_conf.h` 打开；首次编译会多编译 LVGL 的
  snapshot 模块。
- 自动休眠默认 150 秒：调试时如果屏幕已经熄了，先碰一下设备唤醒再双击。

### USB OTA（本地网页升级）

不依赖 WiFi。设备正常运行时通过板载 USB CDC 接收主机发来的固件，写入另一个
OTA app 分区（`default_16MB.csv` 自带 `app0`/`app1`，各 6.25MB，**不需要改
`platformio.ini`**），CRC32 校验通过后自动重启；NVS 里的旋转、亮度设置保留。

**步骤**

```bash
# 1. 编译（产物是 app 镜像，不要用 merged/bootloader 镜像）
cd ESP32-S3-ROUND-DISPLAY
pio run

# 2. 双击 tools/ota/run.bat，或手动起本地页面
py tools/ota/serve.py           # 自动打开 http://localhost:8787/index.html
```

3. 用 USB 线连接设备；若已自动休眠，先触摸屏幕或摇晃唤醒。
4. 页面点“连接设备”，等待显示固件版本和编译时间。首次要选一次串口；之后
   只授权过一个设备时会自动连接（按住 `Shift` 点可按需重新选择）。
5. 拖入 `.pio/build/waveshare_esp32_s3/firmware.bin`，点“开始升级”。
   屏幕显示 `USB OTA` 进度页，完成后自动重启。
6. 升级成功后页面会自动断开，等 10 秒再自动重连（最多 3 次），并对比升级
   前后的编译时间给出结论；想立刻连接可以随时点“连接设备”打断倒计时。

**怎么确认“重启后真的生效”**

页面握手时设备会返回 `OTA: PONG ver=… build=… part=…`：

- `part=` 是当前运行的分区（`app0` / `app1`），每次 OTA 会在两者之间切换；
- `build=` 是 `CFG_FW_BUILD`（`__DATE__ __TIME__`），只有用到它的源文件重新编译
  才会更新（改 `include/config.h` 一定会），所以它没变**不代表**固件没变。

判断以 `part` 为准：`part` 没变说明设备还在原来的槽启动，OTA 没有真正生效；
`part` 变了就是升级成功。想知道选的文件是不是上次那个，看选择文件时的提示——
按文件大小 + CRC32 与上一次成功升级的文件比对。

> 镜像里的 `esp_app_desc`（version / date）不能用来判断版本：这个 PlatformIO
> 配置下它来自预编译的 core 库（`arduino-lib-builder`），每次编译都一样。

**注意**

- 必须用 Chrome / Edge 89+，且通过 `http://localhost` 打开；直接双击
  `index.html`（`file://`）浏览器不会提供 Web Serial。
- `pio device monitor` / 串口助手会独占串口，连接前先关闭。
- 升级期间不要拔 USB。中途取消时设备会在 10 秒超时后自动退出升级模式并
  回到仪表，不需要重启。
- 本 core 打开了 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`，Arduino 会在
  `setup()` 之前就把新镜像标记为有效，所以正常固件不会被回滚；只有在新镜像
  连启动早期都失败时，bootloader 才会回滚到旧固件。真遇到启动即崩溃，用
  `pio run -t upload` / esptool 重新烧录。
- 协议、逐块 ACK 与 CRC 细节见 `tools/ota/README.md`；开关和版本号在
  `include/config.h` 的 `CFG_USB_OTA_ENABLE` / `CFG_FW_VERSION`。

### 设备事件日志（EventLog）

设备长时间无人值守时，断连、配对失败、异常复位、堆内存不足、显示/触摸初始化
失败等事件往往发生在一瞬间，人不在现场、串口监视器也没开，日志就丢了。事件日志
把这类关键事件自动写入板载 Flash，**不依赖 WiFi，也不依赖电脑**；事后通过 USB
连上设备即可读取。

**存储位置**

复用 `default_16MB.csv` 自带的 `spiffs` 分区（起始 `0xC90000`，大小
`0x360000`，约 3.375 MB），在其上挂载 LittleFS，**不需要改分区表，也不需要改
`platformio.ini`**。日志文件：

```text
/littlefs/logs/evt0.log   槽 0
/littlefs/logs/evt1.log   槽 1
/littlefs/logs/evt2.log   槽 2
/littlefs/logs/evt3.log   槽 3
/littlefs/logs/meta.bin   当前槽、全局序号等元数据
```

默认 4 个槽 × 24 KB，硬上限 96 KB（不到分区容量的 3%）。写满后自动轮转覆盖
最旧的槽，**任何情况下都不会把 Flash 写满**；每个槽首行是
`#slot=<n> seq=<n> boot=<n>` 文件头，单独拷出也能看清顺序。

**记录格式**

```text
[000123] 00:12:34.567 W BLE link Ready
[000124] 00:12:35.001 E BLE pairing failed code=5 attempts=3/3
[000125] 00:13:01.220 W HEAP free=28640 min=19200 largest=16384
```

- `[000123]`：全局递增序号，轮转覆盖后也不清零，便于判断先后。
- `00:12:34.567`：本次 boot 的开机毫秒（时:分:秒.毫秒）。设备没有 RTC，所以
  这不是真实时间；**下载文件名用主机当前时间**，方便和现场情况对照。
- `W` / `I` / `E`：级别，`tag` 为来源模块（最长 6 字符），单行上限 160 字节。
- 每次启动写一组 boot banner：boot 号、reset reason、固件版本/编译时间、当前
  运行分区、free heap、wake cause。示例：

```text
===== boot=7 reset=BROWNOUT fw=0.1.7 build=Nov 19 2026 12:00:00 part=app0 heap=182000
[000001] 00:00:00.412 I BOOT wake=ext1 gpio=21
```

**如何读取**

1. 用 USB 连接设备，打开 `tools/ota` 页面（Chrome / Edge，必须是
   `http://localhost`，不能直接双击 `index.html`），点“连接设备”。
2. 在“设备事件日志”卡片点“读取日志”，进度条到 100% 后按 `seq` 从旧到新
   显示各槽内容，槽之间用 `---- slot n (seq=x) ----` 分隔。
3. 点“下载 .txt”保存为 `tesla-log-YYYYMMDD-HHMMSS.txt`（主机本地时间，
   UTF-8 纯文本）；点“清空”二次确认后删除全部日志。

也可以在串口命令行使用同一套命令：

```text
LOG_INFO                              查询挂载状态、容量、当前 seq
LOG_LIST                              列出所有有效槽（按 seq 升序）
LOG_READ <index> <offset> <len>       读一块，len 由设备钳制到 512 字节
LOG_CLEAR                             清空全部日志
LOG_MARK <text>                       手动打一个标记（≤60 字节）
```

读取按 512 字节分块、由页面逐块请求，**不会在连接后自动读取**，避免打扰 OTA
心跳；读取和固件升级不要同时进行（页面会自动互相禁用按钮）。

**埋点范围**

只记录事件，不做全量日志轮询：

- 开机、异常复位（panic / watchdog / brownout 等）；
- BLE 链路状态变化与进入 `Error`，附带加密/配对/重连上下文；
- 配对尝试与失败（`attempts=n/max`）；
- 堆不足（低于阈值记一次，恢复再记一次，带滞回，避免刷屏）；
- 显示、触摸、扩展 IO（TCA9554）初始化失败；
- 自动休眠与唤醒（空闲时长、唤醒源）；
- PWR 长按重启；
- OTA 开始、成功、失败（大小、CRC、失败原因）；
- 手动标记（`LOG_MARK`，现场复现时打时间点）。

**可靠性设计**

- `log()` 只写 4 KB RAM 环形缓冲，不做任何文件 IO；主循环 `evtlog::loop()`
  分块落盘，单次不超过 20 ms（每轮最多 512 字节），不阻塞 LVGL 和 BLE。
- OTA 传输期间暂停落盘，只保留 RAM 缓冲，避免与 `Update.write()` 抢 Flash。
- 直接拔电最多丢最后约 4 KB 未落盘数据；`meta.bin` 用“临时文件 + rename”
  写入，启动时校验失败会自动扫描各槽文件头恢复，不阻塞启动。
- 重复事件按 `tag + 内容` 在 500 ms 内限频，缓冲满时丢弃最旧数据并计数。
- LittleFS 挂载失败时降级为“串口镜像 + 仅 RAM”，`LOG_INFO` 会报告
  `mount=fail`，**不会重启设备**，仪表功能不受影响。

**配置**（`include/config.h`）

```cpp
#define CFG_EVTLOG_ENABLE 1               // 0 = 完全关闭（宏退化为空操作）
#define CFG_EVTLOG_SERIAL_MIRROR 1        // 同时输出到串口
#define CFG_EVTLOG_LEVEL_MIN 0            // 0=I 1=W 2=E
#define CFG_EVTLOG_SLOTS 4                // 槽数量（2..8）
#define CFG_EVTLOG_SLOT_BYTES 24576       // 单槽上限
#define CFG_EVTLOG_RAM_BYTES 4096         // RAM 环形缓冲
#define CFG_EVTLOG_FLUSH_MS 800           // 缓冲超过该时长尝试落盘
#define CFG_EVTLOG_FLUSH_BYTES 256        // 或缓冲达到该字节数落盘
#define CFG_EVTLOG_RATE_LIMIT_MS 500      // 相同事件限频窗口
#define CFG_EVTLOG_HEAP_WARN_BYTES 32768  // 低堆告警阈值
#define CFG_EVTLOG_READ_CHUNK 512         // 单次读取块大小
#define CFG_EVTLOG_TEST_CMDS 0            // 1 = 启用 LOG_TEST 压测，发布保持 0
```

`CFG_EVTLOG_TEST_CMDS` 默认 `0`，只有做轮转/掉电压测时才临时改为 `1`
（启用 `LOG_TEST <n>`），验证完记得改回。

**验证要点**

- 正常上电后读取日志，第一组是 boot banner，包含 boot 号、reset reason、
  `part=` 和 wake cause。
- 关闭车端电源或车辆模拟器：出现 `Reconnecting` / `Error` 及上下文行，且一分钟
  内只有少量条目，不刷屏。
- 长按 PWR 重启后再读取，重启前的最后一条日志仍在。
- 触发自动休眠：睡眠前有休眠记录；运动唤醒后新 boot 日志里有 `wake=ext1`。
- 网页读取进度到 100%，下载的 `.txt` 用记事本/VSCode 打开中文与换行正常。
- 清空后 `LOG_INFO` 返回 `used=0`，再次读取只剩空槽。

### 静止自动休眠 / 运动唤醒

固件支持“设备静止一段时间后熄屏并进入 ESP32-S3 deep sleep，拿起设备后自动唤醒”。
运动只由板载 **QMI8658 IMU** 判断（设备自身是否被移动），不读取车辆速度。

**判定与流程**

- 运行时约每 40 ms 读一次加速度，维护慢速重力估计，用
  `|acc - gravity|` 判断晃动或姿态变化；连续 3 个采样超过阈值才算运动。
- 触摸、BOOT 键、PWR 键操作都会重置静止计时。
- 连续静止达到 `CFG_AUTO_SLEEP_IDLE_MS`（默认 60 秒）后：
  1. 亮度归零并让 CO5300 面板进入 sleep-in；
  2. 把 QMI8658 配置为 Wake-on-Motion，运动时通过 INT2（GPIO21）翻转；
  3. 确认 INT2 已回到低电平后，配置 `ext1 ANY_HIGH` 唤醒并进入 deep sleep。
- 拿起设备 → INT2 拉高 → ESP32 唤醒并**完整重启固件**，因此会重新播放开机动画。
- 启动动画、配钥流程、IMU 不可用时不会休眠；IMU 缺失时自动休眠会保持关闭，
  避免睡下去无法唤醒。

**USB 供电 / 充电**

本机没有电池、长期插 USB 供电，因此 `CFG_AUTO_SLEEP_WHILE_CHARGING` 默认设为
`1`：USB 插入和车辆充电状态都不会阻止休眠。休眠时屏幕、BLE 和串口都会停止，
串口断开是正常现象；运动唤醒后串口会重新枚举。

**唤醒限制**

deep sleep 下 PWR 键不能唤醒 ESP32：PWR 只接 AXP2101 / TCA9554，不在 RTC GPIO
上。当前唤醒源是 QMI8658 INT2（GPIO21）；BOOT 键（GPIO0）可作为后续扩展。

**配置**（`include/config.h`）

```cpp
#define CFG_AUTO_SLEEP_ENABLE 1                        // 0 = 关闭自动休眠
#define CFG_AUTO_SLEEP_IDLE_MS (60UL * 1000UL)         // 静止多久后休眠
#define CFG_AUTO_SLEEP_IMU_ENABLE 1
#define CFG_AUTO_SLEEP_IMU_POLL_MS 40                  // IMU 轮询间隔
#define CFG_AUTO_SLEEP_MOTION_THRESHOLD_MG 50          // 运动判定阈值
#define CFG_AUTO_SLEEP_MOTION_CONFIRM_SAMPLES 3
#define CFG_AUTO_SLEEP_WHILE_CHARGING 1                // 1 = 充电中也休眠
#define CFG_AUTO_SLEEP_WAKE_GPIO 21                    // QMI8658 INT2
#define CFG_AUTO_SLEEP_WOM_THRESHOLD_MG 50             // WoM 唤醒阈值
#define CFG_AUTO_SLEEP_WOM_BLANKING 8
#define CFG_AUTO_SLEEP_WAKE_CONFIRM_MS 0               // 0 = 唤醒后不立即回睡
#define CFG_AUTO_SLEEP_TIMER_WAKE_MS 0                 // 可选定时唤醒兜底
```

`CFG_AUTO_SLEEP_WAKE_CONFIRM_MS` 建议保持 `0`：启动和建界面需要数秒，如果设置
成 2000，确认窗口会在启动过程中过期，可能出现“唤醒后刚显示界面又睡回去”的
现象。调试时如需设备常亮，把 `CFG_AUTO_SLEEP_ENABLE` 改为 `0`。

串口日志示例：

```text
[IMU] QMI8658 ready (id=0x05)
[PWR] wake cause=EXT1 motion=1
[PWR] idle for 60s -> deep sleep
[IMU] WoM configured (threshold=50mg, blanking=8)
[PWR] entering deep sleep; move device to wake
```

BLE 配置在 `include/config.h`：

```cpp
#define CFG_TESLA_VIN "LRWYGCFJ8RC571213"
#define CFG_TESLA_NAME ""        // 留空 = 由 VIN 推导广播名
#define CFG_TESLA_UNITS_MILES 1  // 真车速度/续航原始单位为英里
#define CFG_TESLA_CRYPTO 1       // 加密握手 + GetVehicleData
#define CFG_TESLA_DRIVE_POLL_INTERVAL_MS 100  // 驾驶状态轮询间隔
#define CFG_DEBUG_LOG 1          // 联调日志，稳定后可改 0
```

串口日志会输出前 10 次 Drive 请求从发送到收到响应的耗时（毫秒）及故障码；
请求仍保持串行，因此实际刷新间隔还受车端响应速度影响。

真实私钥放在 `include/secrets/tesla_client_key.h`（已被 `.gitignore` 忽略），
缺失时使用后端内置的测试密钥。工程通过 `lib_extra_dirs = ..` 引用仓库根目录的
`TeslaBleBackend`，无需复制库源码。

## PlatformIO

工程环境登记为 `waveshare_esp32_s3`，使用 ESP32-S3 Dev Module 定义，设置 16 MB Flash、
OPI PSRAM 与 USB CDC。依赖为 LVGL 9、Arduino_GFX（CO5300 QSPI）和 NimBLE-Arduino。微雪示例文档使用
LVGL 8.4；本工程使用 LVGL 9，并配置仅用于仪表盘的 Montserrat 字号。
