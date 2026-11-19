# 设备事件日志（EventLog）功能实施计划

> 目标读者：后续接手实现的 AI / 开发者。
> 本文是**实施计划**，不是已完成代码。按里程碑 P0→P4 顺序做，每个里程碑都有独立验收标准。

---

## 1. 需求

### 1.1 要解决的问题

设备（ESP32-S3 圆屏仪表）在长时间无人值守运行时，偶发问题（断连、配对失败、异常复位、堆内存不足、闪存/显示初始化失败等）只能靠人一直开着串口监视器才能抓到。事发时人不在现场，日志就丢了。

### 1.2 功能目标

1. **特殊事件发生时自动落盘**：不依赖 WiFi、不依赖电脑，事件写入板载 Flash。
2. **空间自动回收（环形轮转）**：日志写满后自动覆盖最旧的，**任何情况下都不会把 Flash 写满**；总占用有硬上限。
3. **可通过现有 OTA 网页读取**：复用 `tools/ota/index.html` 的 Web Serial 通道，新增“读取设备日志 / 下载 / 清空”功能。
4. **不影响实时性**：写 Flash 不在中断/BLE 回调里做；主循环单次文件操作有上限。
5. **可运维**：能看容量、槽位、序号、丢弃计数；能手动打标记（现场复现时点一下）。

### 1.3 非目标（本次不做）

- 不做 WiFi / HTTP / MQTT 上传，不联网。
- 不做完整 printf 日志全量录制（默认只录事件 + WARN/ERROR；可选开关见 §6.3）。
- 不做日志时间同步（设备无 RTC/NTP），用 **boot 序号 + 开机毫秒** 表达时间。
- 不改分区表，不新增第三方库，不改 `platformio.ini`（见 §3 约束）。
- 不复用 ESP-IDF 的 coredump 分区解析（只在启动时记录 reset reason）。

> 注意术语：需求里的“日志回滚”= **环形覆盖（rollover / rotation）**，与 OTA 固件回滚无关，文档统一叫“轮转”。

---

## 2. 现状摸底（实现前先确认）

| 项 | 现状 | 对方案的影响 |
| --- | --- | --- |
| OTA 页面 | `tools/ota/` 本地 `serve.py` + `index.html`，Chrome/Edge Web Serial，纯文本行协议 | 日志读取直接扩展该行协议，不引入 HTTP |
| 协议实现 | `src/ota/UsbOta.cpp` `loop()` 逐行收命令，`handleLine()` 分发；`m_line[96]` 为**输入**行缓冲 | 新命令必须 ≤95 字符/行；输出行不受此限制 |
| 页面解析 | `index.html` `feedText()` 按 `\n` 切行，`onLine()` 按前缀分发；超过 8192 字符的残行会丢弃 | 单条输出行必须 < 2048 字符（建议 ≤ 1700） |
| 串口占用 | OTA 传输期间 `UsbOta::startTransfer()` 阻塞并 `cfglog::setMuted(true)` | 传输期间禁止写日志文件，避免和 `Update.write()` 抢 Flash |
| 文件系统 | **当前未使用**。`board_build.partitions = default_16MB.csv`，标准表里有 3.375 MB 的 `spiffs` 分区（`0xC90000`，`0x360000`） | 直接在该分区上挂 LittleFS，**不用改分区表、不改 ini** |
| LittleFS | ESP32 Arduino core 自带 `LittleFS.h`（2.x/3.x 均有） | 无新增依赖；`LittleFS.begin(false, "/littlefs", 10, "spiffs")` |
| 日志宏 | `config.h` 的 `CFG_LOG` 直接 `Serial.printf`，`cfglog::muted()` 控制静音 | 新事件日志独立于 `CFG_LOG`，但共享“串口镜像”能力 |
| 事件源 | `TeslaUiSource`（链路状态）、`DashboardApp`（按键/触摸/堆）、`PowerManager`（休眠/唤醒）、`UsbOta`（升级） | 在这些位置埋点，见 §5 |
| 休眠 | `PowerManager::enterDeepSleep()` 最后 `esp_deep_sleep_start()`（不返回） | 睡之前必须 `evtlog::flush()` |
| 凌晨/重启 | 上电、`ESP.restart()`、PWR 长按重启、OTA 重启 | 开机写 banner + reset reason |

**P0 必做预检**（不编译不改代码，先确认环境）：

1. 打开 PlatformIO 框架目录下的 `tools/partitions/default_16MB.csv`（通常在
   `C:\Users\<you>\.platformio\packages\framework-arduinoespressif32\tools\partitions\default_16MB.csv`），
   确认存在名为 `spiffs` 的 data 分区（约 0x360000）。
2. 如果不存在（换了 PlatformIO 版本导致），**停止实现并反馈**，改用 §11 备选方案 B，不要擅自改分区表。

> **已在本机确认（写计划时实测，2026-11）**：
>
> - `C:\Users\jeef\.platformio\packages\framework-arduinoespressif32\tools\partitions\default_16MB.csv`
>   内容包含 `spiffs, data, spiffs, 0xc90000, 0x360000`，可用空间 3.375 MB。
> - `framework-arduinoespressif32\libraries\LittleFS\src\LittleFS.h` 存在，签名为
>   `begin(bool formatOnFail=false, const char* basePath="/littlefs", uint8_t maxOpenFiles=10, const char* partitionLabel="spiffs")`，
>   与 §5.4 的调用方式一致，无需新增依赖。
> 后续如升级 PlatformIO/框架包，需重新核对这两点。

---

## 3. 硬性约束（违反则返工）

1. **不要修改 `platformio.ini`**（`AGENTS.md` 明确要求：任何 `build_flags` 变动都会触发 10~17 分钟全量重编）。
   - 只允许改 `include/config.h` 加开关。
   - 不新增 `lib_deps`；用 core 自带 `LittleFS`。
2. **不要改分区表 / 不写 `partitions.csv`**；复用现成 `spiffs` 分区。
3. **不要自动编译/烧录**。改完只汇报“改了哪些文件 + 需要用户验证什么”。
4. **不在中断、定时器回调、NimBLE 回调、LVGL 回调里做文件 IO**。这些位置只能往 RAM 环形缓冲写。
5. **单次主循环内文件操作预算 ≤ 20 ms**（分块写/读，每轮只处理一段）。
6. **OTA 传输期间（`UsbOta::transferring()`）暂停落盘**，只保留 RAM 缓冲（满了丢最旧并计数）。
7. **不引入动态分配**（不用 `String`、`std::vector`、`malloc`），全部静态数组，避免堆碎片影响 LVGL/BLE。
8. **不删除/覆盖任何现有文件内容**；`UsbOta` 原有命令行为保持不变。

---

## 4. 总体方案

```
事件发生（BLE 回调 / 主循环 / 休眠前）
        │  只做内存操作（≤ 几十 µs）
        ▼
┌─────────────────────────────┐
│ RAM 环形缓冲（默认 4 KB）    │  满了丢最旧，累加 dropped 计数
└─────────────┬───────────────┘
              │ DashboardApp::loop() 每轮搬一小段（<20ms）
              ▼
┌─────────────────────────────┐
│ EventLog 落盘（LittleFS）    │  4 个槽 × 24 KB，满槽即轮转覆盖最旧
│ /littlefs/logs/evt0..evt3   │  meta.bin 记录当前槽 + 全局递增 seq
└─────────────┬───────────────┘
              │ USB CDC 文本行协议（复用 OTA 页面）
              ▼
   OTA 网页「设备事件日志」面板：读取 / 下载 .txt / 清空 / 手动标记
```

设计取舍：

- 选择**多槽小文件轮转**而不是“单个大文件内环形覆盖”：LittleFS 不支持原地环形写；文件级轮转实现最简单、掉电最安全、读取排序也直观。
- 选择**文本行 + base64 分块传输**：与现有按 `\n` 切行的页面解析完全兼容；日志里可能出现中文/任意字节，base64 不会破坏行协议。
- 选择**一次请求一块、页面驱动**（非设备一次性推送）：不会长时间占用串口，也不会因为页面来不及消费而丢数据；读 100 KB 约 2~6 秒。

---

## 5. 模块设计

### 5.1 新增文件

| 文件 | 作用 |
| --- | --- |
| `include/log/EventLog.h` | 对外 API：初始化、轮询、写日志、读取/清空/统计、命令处理 |
| `src/log/EventLog.cpp` | 实现：RAM 缓冲、LittleFS 挂载、槽轮转、meta、串口命令 |
| `docs/EVENT_LOG_PLAN.md` | 本文件 |
| （改）`include/config.h` | 新增 `CFG_EVTLOG_*` 开关与参数 |
| （改）`src/app/DashboardApp.cpp/.h` | `begin()` 初始化、`loop()` 轮询、埋点 |
| （改）`src/ota/UsbOta.cpp` | `handleLine()` 转发 `LOG_*` 命令；OTA 事件埋点 |
| （改）`src/data/TeslaUiSource.cpp` | 链路/配对状态变化埋点 |
| （改）`src/power/PowerManager.cpp` | 休眠前 `flush()` + 埋点 |
| （改）`tools/ota/index.html` | 日志面板 + 协议实现 + 下载 |
| （改）`tools/ota/README.md` | 协议表补充 |

### 5.2 头文件接口（建议签名）

```cpp
// include/log/EventLog.h
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace evtlog {

enum class Level : uint8_t { Info = 0, Warn = 1, Error = 2 };

struct Stats {
  bool mounted;        // 文件系统是否可用
  uint8_t slots;       // 槽总数
  uint32_t slotBytes;  // 单槽上限
  uint32_t usedBytes;  // 所有槽实际占用
  uint32_t seq;        // 当前全局序号（最新一条所在槽的序号）
  uint8_t curSlot;     // 正在写的槽
  uint32_t dropped;    // RAM 缓冲满导致的丢弃行数
  uint32_t writeErrors;
};

struct FileInfo {
  uint8_t slot;
  uint32_t seq;
  uint32_t size;
  bool valid;
};

void begin();                      // setup 阶段调用：挂载 + 读 meta + 写开机 banner
void loop();                       // 主循环调用：把 RAM 缓冲分块写盘
void flush();                      // 强制写完（sleep/restart 前）

void log(Level lv, const char* tag, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));
void mark(const char* text);       // 用户标记（来自网页 LOG_MARK）

Stats stats();
uint8_t fileCount();
bool fileInfo(uint8_t index, FileInfo* out);              // index = 0..count-1（按 seq 升序）
int readChunk(uint8_t index, uint32_t offset, uint8_t* dst, size_t len);
void clearAll();

// 串口命令分发：返回 true 表示已处理（由 UsbOta::handleLine 调用）
bool handleCommand(const char* line);
const char* levelName(Level lv);

}  // namespace evtlog

#define EVT_LOGI(tag, fmt, ...) ::evtlog::log(::evtlog::Level::Info, tag, fmt, ##__VA_ARGS__)
#define EVT_LOGW(tag, fmt, ...) ::evtlog::log(::evtlog::Level::Warn, tag, fmt, ##__VA_ARGS__)
#define EVT_LOGE(tag, fmt, ...) ::evtlog::log(::evtlog::Level::Error, tag, fmt, ##__VA_ARGS__)
```

### 5.3 行格式（落盘与串口镜像同一份文本）

```
[000123] 00:12:34.567 W BLE    link Ready
[000124] 00:12:35.001 E BLE    pairing failed code=5 attempts=3/3
[000125] 00:13:01.220 W HEAP   free=28640 min=19200 largest=16384
```

- `[000123]`：全局递增序号（`seq`，单调，轮转不清零，32 位回绕忽略）。
- `00:12:34.567`：本次开机 `millis()` 格式化（`时:分:秒.毫秒`，超过 24h 继续累加小时）。
- `W` / `I` / `E`：级别单字母，省空间。
- tag 最长 6 字符，左对齐补空格。
- 单行硬上限 **160 字节**，超长截断并追加 `…`（保留可读性，避免一条日志吃掉一个槽）。

开机 banner（每个 boot 写一组）：

```
===== boot=7 reset=BROWNOUT fw=0.1.7 build=Nov 19 2026 12:00:00 part=app0 heap=182000
[000001] 00:00:00.412 I BOOT   wake=ext1 gpio=21
```

### 5.4 存储布局与轮转

```
/littlefs/logs/evt0.log   槽 0
/littlefs/logs/evt1.log   槽 1
/littlefs/logs/evt2.log   槽 2
/littlefs/logs/evt3.log   槽 3
/littlefs/logs/meta.bin   8~16 字节：magic + version + curSlot + seq + crc32
```

- 每个日志文件第一行是文件头：`#slot=<n> seq=<n> boot=<n>`，便于单独拷出文件也能看懂顺序。
- 默认 `CFG_EVTLOG_SLOTS = 4`、`CFG_EVTLOG_SLOT_BYTES = 24576` → **硬上限 96 KB**（分区 3.375 MB，占用 <3%）。
- 写流程（`loop()` 内，每轮最多搬 512 B）：
  1. 若缓冲为空 → 返回。
  2. 若当前文件未打开 → 打开 `evt<cur>.log`（`"w"` 截断），写文件头。
  3. 若 `当前文件大小 + 待写长度 > SLOT_BYTES` → 关闭当前文件、`cur = (cur+1) % slots`、`seq++`、重写 meta、开新槽（截断即覆盖最旧）。
  4. `file.write()` 一段 → `file.flush()` 每批一次。
  5. 每写一次 meta 用“临时文件 + rename”方式，避免掉电写坏 meta。
- **meta 损坏/缺失恢复**：校验失败时扫描 `evt*.log`，选择“文件头 seq 最大”的槽继续写；若全都读不出，从槽 0、seq=0 重新开始，不阻塞启动。
- **挂载失败**（分区不存在/损坏）：`mounted=false`，进入“仅 RAM + 串口镜像”降级模式，`LOG_INFO` 里明确报告 `mount=fail reason=...`，绝不 `ESP.restart()`。
- **首次挂载**：`LittleFS.begin(false, ...)` 失败时再 `LittleFS.begin(true, ...)` 格式化一次；仍失败则降级。

### 5.5 RAM 缓冲与并发

- 固定 4096 字节环形字节缓冲 + 读写指针（`std::atomic` 或 `portMUX_TYPE` 临界区，临界区内只做 `memcpy`）。
- 新行放不下 → 丢弃**最旧**未落盘数据（按行边界前移），`dropped++`，并保证下一次落盘时追加一条：
  `[seq] ... W LOG    dropped N lines (buffer full)`。
- 相同 `tag + 消息` 在 `CFG_EVTLOG_RATE_LIMIT_MS`（默认 500 ms）内重复出现 → 只计 suppressed，不重复入队；flush 时汇总一条。
- `mounted=false` 时不尝试打开文件，`loop()` 只做丢弃计数。

---

## 6. 配置项（全部加在 `include/config.h`，不改 ini）

```c
// ---------------------------------------------------------------------------
// 设备事件日志（可持久化，OTA 网页可读取）
// ---------------------------------------------------------------------------
#define CFG_EVTLOG_ENABLE 1
#define CFG_EVTLOG_SERIAL_MIRROR 1        // 同时输出到串口（仍受 cfglog::muted 影响）
#define CFG_EVTLOG_LEVEL_MIN 0            // 0=I 1=W 2=E：低于该级别不落盘
#define CFG_EVTLOG_SLOTS 4                // 槽数量（2..8）
#define CFG_EVTLOG_SLOT_BYTES 24576       // 单槽上限（字节）
#define CFG_EVTLOG_RAM_BYTES 4096         // RAM 缓冲
#define CFG_EVTLOG_FLUSH_MS 800           // 缓冲至少这么久就尝试落盘
#define CFG_EVTLOG_FLUSH_BYTES 256        // 或缓冲达到这么多就落盘
#define CFG_EVTLOG_RATE_LIMIT_MS 500
#define CFG_EVTLOG_HEAP_WARN_BYTES 32768  // 堆低于此值记 WARN（带滞回）
#define CFG_EVTLOG_READ_CHUNK 512         // 单次 LOG_READ 返回的原始字节数
#define CFG_EVTLOG_TEST_CMDS 0            // 1 = 启用 LOG_TEST（压测轮转），发布前置 0
```

关键代码处用 `#if CFG_EVTLOG_ENABLE` 包住，关掉后宏退化为空操作，不增加体积。

### 6.3 可选：镜像全部 `CFG_LOG`（默认关闭）

若后续希望“普通调试日志也留存”，增加 `CFG_EVTLOG_MIRROR_CFG_LOG 1`，把 `CFG_LOG` 宏改成同时调用 `evtlog::log(Info, ...)`。
**本轮不实现**：普通轮询日志量大（20~50 条/秒），会迅速轮转掉真正重要的事件。若要做，必须再加“仅保留最近 N 分钟/仅在异常窗口开启”的触发机制。

---

## 7. 埋点清单（“特殊情况”具体指什么）

按优先级实现，全部是**事件**，不是轮询日志：

| # | 场景 | 埋点位置 | 级别 | 内容 |
| --- | --- | --- | --- | --- |
| 1 | 开机 | `DashboardApp::begin()` 尽早处（`Serial.begin` 之后） | I | reset reason、boot 计数、fw/build、运行分区、free heap、wake cause |
| 2 | 异常复位 | 同上：`reset != POWERON/SW/DEEP_SLEEP` | E | `reset=PANIC/INT_WDT/TASK_WDT/BROWNOUT/...` |
| 3 | 链路状态变化 | `TeslaUiSource::update()` 中 `link != m_lastLink` | I | `Idle→Scanning→Connecting→Syncing→Ready→Reconnecting→Error` |
| 4 | 进入 Error | 同上，`UiLinkState::Error` | E | 附带 `encrypted`、`pairing`、重连次数 |
| 5 | 配对尝试/失败 | `TeslaUiSource` 中 `pairingAttempt` 变化 | W/E | `attempts=n/max`，失败给 E |
| 6 | 事件发生时的上下文 | 同 #4/#5 之后追加一行 | I | `link= heap= uptime= speed= soc= rssi=`（只取已有字段，不新增 BLE 请求） |
| 7 | 堆不足 | `DashboardApp::loop()` 每 5 s 采样 | W | 低于阈值（带 8 KB 滞回，每次 boot 只报进入/恢复各一次） |
| 8 | 显示/触摸/扩展 IO 初始化失败 | 已有 `CFG_LOG` 的失败分支（`RoundDisplay`/`Cst9217Touch`/TCA9554） | E | 把对应失败日志改为 `EVT_LOGE` |
| 9 | 自动休眠/唤醒 | `PowerManager::enterDeepSleep()` 开头、`detectWakeReason()` | I | 空闲时长、唤醒源；休眠前先 `flush()` |
| 10 | 用户长按重启 | `DashboardApp` PWR 长按分支 | W | `restart by user hold=3000ms`，重启前 `flush()` |
| 11 | OTA 开始/成功/失败 | `UsbOta::startTransfer()` 关键分支 | I/E | size、crc、失败原因；**传输中不写盘**，结束后统一落盘 |
| 12 | 网页手动标记 | `LOG_MARK <text>` 命令 | I | 现场复现时打时间点，便于对照 |
| 13 | 读取/清空操作 | `LOG_CLEAR` / `LOG_READ` 结束 | I | 审计用，可选 |

实现要求：

- 事件必须**先去重/限频**，禁止在 BLE 回调里每秒刷几十条。
- `EVT_LOG*` 内部自行判断 `CFG_EVTLOG_ENABLE` 与 `CFG_EVTLOG_LEVEL_MIN`；调用处不必写 `#if`（但高频路径可加编译期开关）。
- 不要在 `TeslaBleBackend` 库里加依赖；只用它的状态回调/已有 `UiModel` 字段。若某字段拿不到，就留空，不扩接口。

---

## 8. 串口协议扩展（与 OTA 页面）

### 8.1 命令表（主机 → 设备，`\n` 结尾，全部 ≤95 字符）

| 命令 | 说明 |
| --- | --- |
| `LOG_INFO` | 查询状态 |
| `LOG_LIST` | 列出所有有效槽（按 seq 升序） |
| `LOG_READ <index> <offset> <len>` | 读一块；`len` 由设备钳制到 `CFG_EVTLOG_READ_CHUNK`(512) |
| `LOG_CLEAR` | 清空全部日志（重建 meta，seq 不清零或清零由实现定，建议**清零**并记录 clear 事件到新文件） |
| `LOG_MARK <text>` | 写入一条用户标记（text 取整行剩余部分，最长 ~60 字节） |
| `LOG_TEST <n>` | 仅 `CFG_EVTLOG_TEST_CMDS=1` 时可用：生成 n 行测试日志验证轮转 |

### 8.2 响应（设备 → 主机，`OTA:` 前缀，单行 < 2048 字符）

```
OTA: LOG INFO enabled=1 mount=ok slots=4 slotBytes=24576 used=3120 seq=42 cur=2 dropped=0 err=0
OTA: LOG FILE slot=0 seq=39 size=24576
OTA: LOG FILE slot=2 seq=42 size=3120
OTA: LOG LIST done files=2
OTA: LOG DATA slot=2 offset=0 len=512 crc=89abcdef <base64>
OTA: LOG READ end
OTA: LOG CLEAR ok
OTA: LOG MARK ok
OTA: LOG ERR <reason>
```

约定：

- `LOG_DATA` 每行带**块内 CRC32**（与页面现有 `crc32()` 同算法）；页面校验失败就重试该块一次。
- base64 用标准字母表 + `=` 填充，无换行。
- 页面读取流程：`LOG_LIST` → 对每个文件从 `offset=0` 循环 `LOG_READ` 到 `size` → 拼接 → 校验完成后渲染/下载。
- 每次只允许一个未完成的 `LOG_*` 请求（页面侧用 `busy`/Promise 串行化），避免和设备 `m_line` 缓冲打架。
- `LOG_ERR` 一律用 `OTA: LOG ERR ...`，**不要**用 `OTA: ERR`——后者会触发页面 `rejectAll()` 把正在进行的 OTA 升级流程也中断掉。

### 8.3 与 `UsbOta` 的接线

`UsbOta::handleLine()` 开头（`OTA_PING` 之前即可）：

```cpp
if (evtlog::handleCommand(line)) return;
```

`handleLine` 运行在主循环上下文，且 OTA 传输期间不会进入（阻塞在 `startTransfer`），因此 `LOG_*` 的文件 IO 与 `Update.write()` 不会并发。

`LOG_MARK` 输入走 `m_line[96]`，注意实现里对超长行已有“整行丢弃”逻辑，文档中说明限制即可。

---

## 9. 网页改动（`tools/ota/index.html`）

### 9.1 UI

在“设备信息 / 固件升级”之后新增一张卡片：

```
┌ 设备事件日志 ────────────────────────────────┐
│ [读取日志] [下载 .txt] [清空]   容量 3.0/96 KB │
│ 进度：████████░░░░ 45%   （读取时显示）        │
│ <pre id="evtlog">  [07-12 18:20:03] boot=... │
│ ...                                          │
└──────────────────────────────────────────────┘
```

要求：

- “读取日志”前必须先连通设备；未连接时按钮禁用。
- 升级进行中（`busy`）禁用全部日志按钮。
- 读取时**禁用开始升级**，避免命令交错。
- 面板只展示文本，不 `innerHTML` 拼接（防止日志内容被当 HTML 解析）。
- 显示时把 4 个槽按 `seq` 升序拼接；每个槽之间插入一行 `---- slot n (seq) ----` 分隔。
- “下载 .txt”：`Blob` + `URL.createObjectURL`，文件名 `tesla-log-YYYYMMDD-HHMMSS.txt`。
- 清空前二次确认（`confirm()` 即可）。
- 连接后**不要自动读取**（避免干扰 OTA 心跳/升级），仅刷新 `LOG_INFO` 容量显示（可选）。

### 9.2 页面解析

- 顶部 `PAGE_VERSION` 递增（例如 `ota-page-10`）。
- `onLine()` 新增分支，顺序放在通用 `line.startsWith('OTA:')` 处理之前：

```js
if (line.startsWith('OTA: LOG DATA ')) { handleLogData(line); return; }
if (line.startsWith('OTA: LOG FILE ')) { handleLogFile(line); return; }
if (line.startsWith('OTA: LOG INFO ')) { handleLogInfo(line); return; }
if (line.startsWith('OTA: LOG '))     { dispatchLine(line); return; }  // LIST done / READ end / CLEAR ok / MARK ok / ERR
```

- `handleLogData`：解析 `slot/offset/len/crc`，`atob` → `Uint8Array`，用现有 `crc32()` 校验，追加到读取缓冲。**不要**调用 `addLog`，否则几千行会刷爆串口日志面板。
- 读取循环示例（串行、带超时）：

```js
for (const f of files) {
  let off = 0;
  while (off < f.size) {
    const line = await waitFor(/^OTA: LOG DATA /, 3000, '读日志');
    // 校验并追加，off += len
  }
}
```

- 所有 `LOG_` 响应使用独立 `waitFor` 正则，避免与 `OTA: LOG INFO` 互相吞掉。
- `feedText()` 的 8192 字符残行保护保持不动；单行 base64 ≤ 700 字符即可。

### 9.3 README

在 `tools/ota/README.md` 的协议表里补上 §8 的命令与响应，并增加“读取设备事件日志”操作步骤。

---

## 10. 里程碑与验收

### P0 预检（0.5 h）

- [ ] 确认 `default_16MB.csv` 有 `spiffs` 分区（记录路径/大小到 PR 描述或交付说明）。
- [ ] 确认 `LittleFS.h` 在当前 core 下可用（看 framework 目录即可，不必编译）。
- 产出：一句结论 + 若不可用则转 §11 备选 B。

### P1 存储内核 + 命令（核心，先可跑通）

- 新增 `EventLog.h/.cpp`：挂载、RAM 缓冲、槽轮转、meta、`log()`、`loop()`、`flush()`、`stats()`、`fileInfo()`、`readChunk()`、`clearAll()`、`handleCommand()`。
- `config.h` 加 `CFG_EVTLOG_*`。
- `DashboardApp::begin()` 调 `evtlog::begin()`；`DashboardApp::loop()` 调 `evtlog::loop()`。
- `UsbOta::handleLine()` 转发 `LOG_*`。
- `LOG_TEST`（默认关）用于压测。

验收：

1. 串口发 `LOG_INFO` 返回 `mount=ok`、`slots=4`。
2. `LOG_TEST 100` 后 `LOG_LIST` 能看到文件；串口镜像有对应行。
3. `LOG_READ` 按块读回的内容与写入一致（CRC 全过）。
4. 反复 `LOG_TEST` 直到超过 4×24 KB：`used` 不超过 96 KB，`seq` 持续增长，最旧槽被覆盖，设备不卡顿、不复位。
5. 拔电重启后 `LOG_LIST` 仍能看到历史文件，且 boot 号 +1。

### P2 埋点接入

- 按 §7 的 #1~#11 接入；#12 随 P1 已有。
- 把已有失败分支（显示/触摸/扩展 IO）改成 `EVT_LOGE`（串口输出保持原样）。

验收：

1. 正常上电：日志里有一条 boot banner + wake cause。
2. 断开车端电源或关掉车辆模拟器：出现 `link ... Reconnecting/Error` + 上下文行，且**不刷屏**（1 分钟内 ≤ 少量条目）。
3. 拔掉触摸排线（或改 `CFG_DEMO_MODE` 不现实时，用注入方式模拟）出现一次 E 级初始化失败记录。
4. 长按 PWR 重启前，最后一条日志完整落盘（重启后能读到）。
5. 睡眠→运动唤醒：睡眠前有记录，唤醒后有 `wake=ext1`。

### P3 网页读取/下载/清空

- 按 §9 改造页面 + README；`PAGE_VERSION` 递增。

验收：

1. 连接后点“读取日志”，进度到 100%，面板显示完整文本，与串口镜像一致。
2. 下载的 `.txt` 用记事本/VSCode 打开正常（UTF-8），槽分隔正确、顺序从旧到新。
3. 点“清空”后 `LOG_INFO` 的 `used=0`，重新读取只有空槽。
4. 读取中途点“断开”：页面不崩、无未捕获 Promise；设备不重启、不卡死。
5. 升级过程中日志按钮禁用；升级完成后日志仍可读（新 boot banner 在）。

### P4 稳定性与收尾

- 检查所有高频路径是否限频。
- 检查无 `String`/`new`/大栈数组。
- OTA 期间落盘暂停逻辑验证：升级 + 同时产生 BLE 事件时，升级不失败，事件在升级后补写（或在 RAM 满时计 dropped）。
- 更新 `ESP32-S3-ROUND-DISPLAY/README.md`（一节“事件日志”+ 如何读取）。
- 关掉 `CFG_EVTLOG_TEST_CMDS`；确认默认配置可直接发版。

验收（压力）：

1. 连续 `LOG_TEST` 50 次（每次 500 行）后：无复位、`used` 稳定在上限、`dropped=0`（或仅测试模式下合理值）。
2. 写入过程中直接拔电 10 次：每次重启后都能挂载成功，最坏丢最后 4 KB 缓冲，不出现“格式损坏/无限格式化”。
3. OTA 升级 3 次：升级成功率不变，日志读取在新固件上可用。

---

## 11. 风险与备选方案

| 风险 | 影响 | 对策 |
| --- | --- | --- |
| 分区表里没有 `spiffs` | 无法挂载 | 备选 A：改用 `LittleFS.begin(...,"nvs")`（不可行，NVS 太小）→ **备选 B：向用户申请新增一个 `logfs` 分区并改分区表**（需用户同意，属破坏性变更，本计划默认不做） |
| LittleFS 首次挂载失败 | 日志不可用 | 自动格式化一次；仍失败则降级为串口镜像 + `mount=fail`，不影响仪表功能 |
| Flash 磨损 | 寿命 | 只写事件、4 KB 缓冲批量写、96 KB 上限；按每天 100 条 × 100 B ≈ 10 KB 估算，写入放大小，寿命足够 |
| 掉电写坏 meta / 文件 | 启动异常 | meta 用 tmp+rename；启动时容错扫描；文件按行解析，末尾残行丢弃 |
| 读取大日志时阻塞 LVGL | 界面卡顿 | 分块读取，每块 ≤512 B；页面逐块请求；主循环 `evtlog::loop()` 每轮只搬 512 B |
| 页面 `lineBuf` 8 KB 上限 | 丢数据 | base64 单行 ≤ 700 字符（512 B 原始数据） |
| 日志里出现 `\n` / 非 UTF-8 | 页面解析错位 | 落盘前把 `\r\n` 转义为 `\\n`，非法 UTF-8 字节替换为 `?`；传输再套 base64 |
| `LOG_MARK` 超长 | 被 `m_line` 丢弃 | 文档说明 ≤60 字节；页面输入框 `maxlength` |
| 与 OTA 争抢 Flash | 升级失败 | `UsbOta::transferring()` 时暂停落盘；OTA 结束/失败后恢复 |
| 多任务写缓冲 | 数据竞争 | 只用临界区保护 RAM 环形缓冲；文件只在主循环操作 |
| 时间不可信 | 无法对应真实时间 | 明确用 boot+uptime；下载文件名用**主机当前时间**，页面可另存一份 host 时间戳表头 |

---

## 12. 交付物清单（实现完成后应存在）

1. `include/log/EventLog.h`、`src/log/EventLog.cpp`
2. `include/config.h` 中 `CFG_EVTLOG_*` 段（带注释）
3. `src/app/DashboardApp.cpp/.h`、`src/ota/UsbOta.cpp`、`src/data/TeslaUiSource.cpp`、`src/power/PowerManager.cpp` 的埋点
4. `tools/ota/index.html` 日志面板 + 协议（版本号已递增）
5. `tools/ota/README.md` 协议/操作补充
6. `ESP32-S3-ROUND-DISPLAY/README.md` 新增“设备事件日志”章节
7. 用户验证记录（按 §10 各里程碑勾选）

> 交付时请明确列出：改了哪些文件、新增了哪些配置项、**需要用户自己执行 `pio run` 并烧录**、以及 §10 中哪几条需要上板验证。

---

## 13. 实现顺序建议（给 AI 的执行提示）

1. 先读 `include/config.h`、`src/ota/UsbOta.cpp`、`src/app/DashboardApp.cpp`、`tools/ota/index.html` 的现有实现，保持风格一致（中文注释、`CFG_` 前缀、`namespace`/类封装）。
2. 按 P1 → P2 → P3 顺序改，**每个里程碑结束停下来让用户烧录验证**，不要一次性写完。
3. 任何需要改 `platformio.ini` / 分区表的情况，先停下来问用户。
4. 不要执行编译命令；只做静态自检（括号、头文件包含、`#if` 配对、字符串转义）。
5. 所有新增注释用中文，解释“为什么”而不是“是什么”。
