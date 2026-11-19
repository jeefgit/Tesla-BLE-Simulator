# 车辆 VIN 运行时输入设计（圆屏固件）

> 状态：设计稿，未实现  
> 目标工程：`ESP32-S3-ROUND-DISPLAY`（466×466 圆屏、CST9217 触摸）  
> 后端库：`TeslaBleBackend`（`TeslaBleSource` / `TeslaClient` / `TeslaCrypto`）  
> 关联文档：`simulator/docs/PAIRING.md`、`simulator/docs/ARCHITECTURE.md`

---

## 1. 背景与目标

### 1.1 现状

当前整车 VIN 是**编译期配置**：

```c
// ESP32-S3-ROUND-DISPLAY/include/config.h
#define CFG_TESLA_VIN "LRWYGCFJ8RC571213"
#define CFG_TESLA_NAME ""   // 留空 = 由 VIN 推导广播名
```

VIN 在三个地方被使用：

1. **BLE 广播名**
   `TeslaBleSource::begin()` 用 `TeslaClient::advertisedNameFromVin(VIN)` 计算
   `S + sha1(VIN)[:16] + C`，再 `BleLink::setTargetName()` 精确匹配车辆。
2. **加密握手元数据**
   `TeslaClient::begin()` 调用 `m_crypto.begin(String(CFG_TESLA_VIN))`；
   `TeslaCrypto` 在 SessionInfo / 请求 / 响应元数据里写入 `TAG_PERSONALIZATION = VIN`。
   如果 VIN 与真实车辆不一致，握手认证必然失败。
3. **UI 展示**
   `TeslaUiSource::begin()` 把 `CFG_TESLA_VIN` 拷进 `UiModel.vin`。
   但当前 `LvglDashboard` **没有渲染** `model.vin`，链接页只显示 `ELAPSED`。

因此现在换车必须：改 `config.h` → 重新编译 → 烧录。

### 1.2 目标

- 不重新编译，直接在设备上输入 17 位 VIN。
- VIN 掉电保存（NVS），OTA 升级后仍保留。
- 输入后自动应用到：BLE 广播名、加密握手 VIN、UI 展示。
- 保留 `CFG_TESLA_VIN` 作为**出厂默认值**。
- 不改 `platformio.ini`，不引入 LVGL 输入设备（现有触摸是 DashboardApp 手动轮询）。
- 输入流程简单、可取消、有校验、有错误提示。
- 不把完整 VIN 写进串口/事件日志（隐私）。

### 1.3 非目标（本期不做）

- 修改 `TESLA-DASHBOARD`（CYD 屏幕）固件；它的 VIN 仍为编译期配置。
- 通过 Web OTA 页面输入 VIN（见 §10 备选方案）。
- 多车 VIN 管理 / 收藏列表。
- 修改配对密钥（本固件的客户端私钥仍来自 `secrets/tesla_client_key.h`）。

---

## 2. 方案总览

### 2.1 推荐方案（Phase 1）

**NVS 覆盖 + 设备端字母/数字键盘 + 保存后自动重启。**

```
启动
  └─ vinstore::begin()
        ├─ 读取 NVS namespace "tesla", key "vin"
        ├─ 合法（17 位 A-Z0-9）→ 使用
        └─ 不存在/非法 → 回退 CFG_TESLA_VIN
  └─ TeslaUiSource::begin()
        ├─ model.vin = vinstore::current()
        └─ backend.setVin(vinstore::current()) → begin()

用户输入
  └─ 链接页点击 VIN / 仪表页长按顶部
  └─ 打开 VIN 编辑器（暂停 BLE 轮询与仪表重绘）
  └─ 17 位后 OK
  └─ vinstore::save() 写 NVS
  └─ 显示 SAVED，约 900ms 后 ESP.restart()
  └─ 重启后按新 VIN 扫描、握手、展示
```

**为什么保存后重启？**

- BLE 扫描目标名、已建立连接、`TeslaCrypto` 会话、配对提示状态都需要一起失效；
  重启是最简单、最不容易出竞态的实现。
- VIN 属于一次性配置，不需要秒级热切换。
- Phase 2 如需“不重启热切换”，再按 §9 的接口扩展。

### 2.2 优先级

```text
NVS 中的 "tesla/vin"  >  CFG_TESLA_VIN（出厂默认）
```

`CFG_TESLA_NAME` 保持为空。若它非空，会覆盖 VIN 推导的广播名，**此时改 VIN
不影响扫描目标**；实现时需在日志里明确警告。

### 2.3 校验规则

- 长度必须 **17**。
- 字符只允许 `A-Z`、`0-9`（键盘只输出大写）。
- VIN 标准不允许 `I`、`O`、`Q`。建议：
  - 默认**不阻断**，仅提示 `VIN CONTAINS I/O/Q`（方便模拟器/测试 VIN）；
  - 可选 `CFG_VIN_STRICT_ISO=1` 时直接拒绝。
- 保存前统一转大写；NVS 中只存大写。
- 空字符串视为“无 VIN”，后端退回前缀扫描，但 UI 显示 `VIN NOT SET`。

---

## 3. 新增模块：VinStore

### 3.1 文件

- 新增 `include/config/VinStore.h`
- 新增 `src/config/VinStore.cpp`

> PlatformIO 已通过 `-I include` 引用头文件，`src/` 会递归编译，不需要改 `platformio.ini`。

### 3.2 接口

```cpp
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace vinstore {

constexpr size_t kLength = 17;
constexpr size_t kCapacity = kLength + 1;

enum class Error : uint8_t {
  None = 0,
  Empty,
  Length,
  InvalidChar,
  StrictIso,
  NvsWrite,
};

// 启动时调用一次：加载 NVS 覆盖值，非法则回退 CFG_TESLA_VIN。
void begin();

// 当前生效 VIN；可能为空字符串（配置也无效且 NVS 无值）。
const char* current();

// 当前是否没有 NVS 覆盖（即使用出厂默认值）。
bool isDefault();

// 纯校验：长度 17、A-Z0-9，可选 I/O/Q 检查。
Error validate(const char* vin);

// 大写规范化 + 校验。out 必须至少 kCapacity 字节。
Error normalize(const char* in, char out[kCapacity]);

// 校验并写 NVS，成功后同步更新 current()。
Error save(const char* vin);

// 删除 NVS 覆盖，恢复出厂默认值。
bool clear();

// 给 UI 用的英文短提示，例如 "VIN MUST BE 17 CHARACTERS"。
const char* errorText(Error error);

}  // namespace vinstore
```

### 3.3 实现要点

```cpp
namespace {
constexpr char kPrefsNamespace[] = "tesla";
constexpr char kPrefsVinKey[] = "vin";
char s_vin[vinstore::kCapacity] = {};
bool s_isDefault = true;

bool readNvs(char out[vinstore::kCapacity]) {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, true)) return false;
  const size_t n = prefs.getString(kPrefsVinKey, out, vinstore::kCapacity);
  prefs.end();
  return n == vinstore::kLength;
}
}  // namespace
```

- `begin()`：
  1. 先 `readNvs()`，`validate()` 通过则使用，`s_isDefault=false`；
  2. 否则读取 `CFG_TESLA_VIN`，合法则使用，`s_isDefault=true`；
  3. 两者都非法时 `s_vin[0]='\0'`，串口/事件日志记一条 WARN（**不要打印完整 VIN**）；
  4. 日志示例：`[VIN] source=nvs tail=571213` 或 `[VIN] source=default tail=571213`。
- `normalize()`：
  - 逐字符 `toupper`；
  - 只接受 `A-Z0-9`；
  - 长度不足/超出返回 `Length`；
  - `CFG_VIN_STRICT_ISO` 为 1 时遇到 `I/O/Q` 返回 `StrictIso`。
- `save()`：
  - `normalize()` 失败直接返回错误；
  - `prefs.begin(ns, false)` → `putString("vin", normalized)` → `end()`；
  - 读回校验（`getString` 长度与内容一致），失败返回 `NvsWrite`；
  - 成功后 `strcpy(s_vin, normalized)`、`s_isDefault=false`；
  - 若新 VIN 与 `CFG_TESLA_VIN` 完全相同，可直接 `clear()`，避免 NVS 里存冗余值。
- `current()` 永远返回非空指针（可能指向 `""`）。
- 头文件里不要 `#include <Preferences.h>` / `config.h`，只在 `.cpp` 中包含。
- `VinStore.cpp` 需要 `#include "config.h"` 读取 `CFG_TESLA_VIN` 与 `CFG_VIN_STRICT_ISO`。
- 日志使用 `EVT_LOGI/EVT_LOGW`（如果实现时 EventLog 已初始化）或 `CFG_LOG`。

---

## 4. 后端改造：让 VIN 成为运行期参数

### 4.1 `TeslaBleSource`

`include/data/TeslaBleSource.h`：

```cpp
class TeslaBleSource : public IVehicleSource {
 public:
  void begin() override;
  void loop() override;

  // 必须在 begin() 前调用；空指针/空串则回退 CFG_TESLA_VIN。
  void setVin(const char* vin);

  // Phase 2 可选：运行期热切换（见 §9），Phase 1 不需要实现。
  // void applyVin(const char* vin);

  // ...

 private:
  BleLink m_link;
  TeslaClient m_client;
  String m_vin;
};
```

`src/data/TeslaBleSource.cpp`：

```cpp
void TeslaBleSource::setVin(const char* vin) {
  m_vin = vin ? vin : "";
}

void TeslaBleSource::begin() {
  if (m_vin.length() == 0) m_vin = CFG_TESLA_VIN;

  m_link.begin();

  String targetName =
      strlen(CFG_TESLA_NAME) > 0
          ? String(CFG_TESLA_NAME)
          : TeslaClient::advertisedNameFromVin(m_vin.c_str());
  if (targetName.length() == 0) {
    CFG_LOG("广播名推导失败（VIN 为空？），退回前缀/UUID 扫描");
  } else {
    m_link.setTargetName(targetName);
    // tail6() 是文件内小工具：返回字符串最后 6 个字符；长度不足时返回原串。
    CFG_LOG("目标 VIN 尾号=%s 广播名=%s", tail6(m_vin.c_str()),
            targetName.c_str());
  }

  m_link.setRxHandler([this](const uint8_t* data, size_t len) {
    m_client.feed(data, len);
  });
  m_client.begin(&m_link, m_vin.c_str());
}
```

> 后端库不能依赖固件的 `config/VinStore.h`（`TESLA-DASHBOARD` 也共用该库），
> 所以 VIN 必须由上层通过 `setVin()` 注入。

### 4.2 `TeslaClient`

`include/tesla/TeslaClient.h`：

```cpp
void begin(BleLink* link, const char* vin);

// Phase 2 可选
void setVin(const char* vin);
```

`src/tesla/TeslaClient.cpp`：

```cpp
void TeslaClient::begin(BleLink* link, const char* vin) {
  m_link = link;
  // ...原有初始化...

  const char* effective = (vin && vin[0] != '\0') ? vin : CFG_TESLA_VIN;
  m_crypto.begin(String(effective));
  // ...原有 handler 注册...

  // tail6() 同上：只打印后 6 位，不打印完整 VIN。
  CFG_LOG("特斯拉加密协议栈就绪 VIN 尾号=%s（握手后改用加密 GetVehicleData 轮询）",
          tail6(effective));
}
```

注意：

- `TeslaCrypto::begin()` 内部会 `reset()`，所以 VIN 变更后旧会话自然失效。
- 不要在其他地方再直接引用 `CFG_TESLA_VIN`（除默认值回退外）。
  实现后全局搜索确认：
  ```text
  CFG_TESLA_VIN
  ```
  应只出现在 `config.h`、`BackendConfig.h` 默认宏、`VinStore.cpp` 回退、
  `TeslaClient.cpp` 回退、`TeslaBleSource.cpp` 回退。

### 4.3 `TeslaUiSource`

`src/data/TeslaUiSource.cpp`：

```cpp
#include "config/VinStore.h"

void TeslaUiSource::begin() {
  m_stateSinceMs = millis();

  const char* vin = vinstore::current();
  m_model.vin[0] = '\0';
  if (strlen(vin) < sizeof(m_model.vin)) {
    strncpy(m_model.vin, vin, sizeof(m_model.vin) - 1);
    m_model.vin[sizeof(m_model.vin) - 1] = '\0';
  }

  m_backend.setVin(vin);
  m_backend.begin();
}
```

`DashboardApp::begin()` 必须在 `m_source.begin()` 之前调用 `vinstore::begin()`。
建议顺序：

```cpp
void DashboardApp::begin() {
  evtlog::begin();
  vinstore::begin();      // 尽早加载，后续所有模块都读同一个值
  // ...
}
```

> `vinstore::begin()` 内部使用 NVS，不依赖显示/LVGL，放在 `evtlog::begin()`
> 之后即可。不要放在 `display.begin()` 之后，否则 UI 第一次渲染拿不到值。

---

## 5. UI 设计

### 5.1 入口

| 当前页面 | 操作 | 说明 |
|---|---|---|
| 链接页（未 Ready / 断连） | 点击底部 VIN 行 | VIN 行显示 `VIN LRWYG...` 与 `TAP TO CHANGE` |
| 仪表页（Ready） | 长按顶部区域 1.2s | 顶部区域 `y < 70`，位移 < 20px，避免和亮度手势冲突 |
| 任何页面 | 不提供 | 不做三击/隐藏手势，降低误触 |

长按判定只在 `m_model.link == UiLinkState::Ready` 时启用；
链接页始终可点 VIN 行。

### 5.2 链接页 VIN 行

在 `LvglDashboard::buildLinkPage()` 现有 `m_linkMeta`（`ELAPSED`，y=333）下方增加：

```text
y=350  VIN LRWYGCFJ8RC571213        字体 montserrat_12，kFaint，居中
y=370  TAP VIN TO CHANGE             字体 montserrat_10，kFaint，居中
```

坐标建议：

```cpp
m_linkVin = makeLabel(m_linkPage, 40, 350, 386, 18, "VIN --",
                      &lv_font_montserrat_12, kFaint, LV_TEXT_ALIGN_CENTER);
m_linkVinHint = makeLabel(m_linkPage, 46, 369, 374, 16, "TAP VIN TO CHANGE",
                          &lv_font_montserrat_10, kFaint, LV_TEXT_ALIGN_CENTER);
```

在 `render()` 的 `showLinkPage` 分支：

```cpp
char vinText[32];
if (model.vin[0] == '\0') {
  snprintf(vinText, sizeof(vinText), "VIN NOT SET");
  lv_obj_set_style_text_color(m_linkVin, kAmber, 0);
} else {
  snprintf(vinText, sizeof(vinText), "VIN %s", model.vin);
  lv_obj_set_style_text_color(m_linkVin, kFaint, 0);
}
updateLabel(m_linkVin, vinText);
```

**点击热区**（DashboardApp 侧，逻辑坐标）：

```text
x: 34..432, y: 344..390
```

### 5.3 VIN 编辑器布局

全屏覆盖层，创建在 `LvglDashboard::begin()` 最后（Boot 动画层之上），默认隐藏：

```text
┌──────────────────────────────┐  466×466
│          VEHICLE VIN         │  y=12, montserrat_10, kDim, 居中
│     LRWYGCFJ8RC571213        │  y=30, montserrat_24, kText, 居中
│         17 / 17              │  y=63, montserrat_10, kDim, 居中
│                              │
│   A   B   C   D   E   F      │  key grid: 6×5
│   G   H   I   J   K   L      │  key 52×52, pitch 58
│   M   N   O   P   Q   R      │  origin (62, 91)
│   S   T   U   V   W   X      │
│   Y   Z  DEL 123  OK   X     │
│                              │
│      TAP A KEY TO EDIT       │  y=388, montserrat_10, kFaint
└──────────────────────────────┘
```

几何参数：

```cpp
constexpr int32_t kVinScreen = 466;
constexpr int32_t kVinKey = 52;
constexpr int32_t kVinPitch = 58;       // 52 + 6 gap
constexpr int32_t kVinGridCols = 6;
constexpr int32_t kVinGridRows = 5;
constexpr int32_t kVinGridX = 62;       // (466 - 342) / 2
constexpr int32_t kVinGridY = 91;       // 居中于 466 高度
constexpr int32_t kVinKeyCount = 30;
```

圆屏安全检查：网格右下半角相对圆心 `(233,233)` 的距离为
`sqrt(171² + 142²) ≈ 223 < 233`，所有按键都在圆形显示区内。

### 5.4 按键映射

字母页（默认）：

| 索引 | 0..25 | 26 | 27 | 28 | 29 |
|---|---|---|---|---|---|
| 显示 | `A`..`Z` | `DEL` | `123` | `OK` | `X` |
| 行为 | 追加字符 | 删除最后一个 | 切数字页 | 提交 | 取消 |

数字页：

| 索引 | 0..9 | 10 | 11 | 12 | 13 | 14..29 |
|---|---|---|---|---|---|---|
| 显示 | `1`..`9`,`0` | `DEL` | `ABC` | `OK` | `X` | 隐藏 |
| 行为 | 追加数字 | 删除 | 切字母页 | 提交 | 取消 | 不可点 |

- 所有按键文字只用 ASCII，避免 Montserrat 缺字形。
- `OK` 在长度不足 17 时样式变暗（文字 `kFaint`、边框 `kTrack`），但点击仍给出错误提示。
- `X` 永远可用；取消不写 NVS。
- 编辑缓冲区初始值 = 当前 VIN（方便修改，而不是从空开始）。
- 达到 17 位后继续按字符键直接忽略，不覆盖已有内容。
- 键盘对象只在 `begin()` 创建一次；`show/hide` 只切换隐藏标志并更新文字，
  避免反复创建 LVGL 对象造成堆碎片。

### 5.5 `LvglDashboard` 新增接口

`include/ui/LvglDashboard.h`：

```cpp
#include <functional>

class LvglDashboard : public IDashboard {
 public:
  // ...原有...

  // 链接页是否可见（供 DashboardApp 判断 VIN 行点击热区）。
  bool linkPageVisible() const;

  void vinEditorShow(const char* initialVin);
  void vinEditorHide();
  bool vinEditorVisible() const { return m_vinEditorVisible; }

  // 返回 true 表示触摸已被编辑器消费。
  bool vinEditorHandleTap(int16_t x, int16_t y);

  void vinEditorSetError(const char* text);   // 红色错误行
  void vinEditorShowSaved();                  // "SAVED - RESTARTING..."

  void vinEditorSetSubmitHandler(std::function<void(const char*)> handler);
  void vinEditorSetCancelHandler(std::function<void()> handler);

 private:
  void buildVinEditor();
  void vinEditorSetLayout(bool numeric);
  void vinEditorRefresh();
  int  vinHitKey(int16_t x, int16_t y) const;
  void vinEditorSubmitOrError();

  // ...原有成员...

  lv_obj_t* m_linkVin = nullptr;
  lv_obj_t* m_linkVinHint = nullptr;

  lv_obj_t* m_vinEditor = nullptr;
  lv_obj_t* m_vinTitle = nullptr;
  lv_obj_t* m_vinText = nullptr;
  lv_obj_t* m_vinCounter = nullptr;
  lv_obj_t* m_vinStatus = nullptr;
  lv_obj_t* m_vinKeys[30] = {};
  lv_obj_t* m_vinKeyLabels[30] = {};
  bool m_vinKeyVisible[30] = {};
  bool m_vinEditorVisible = false;
  bool m_vinNumericLayout = false;
  char m_vinEdit[18] = {};
  uint8_t m_vinEditLen = 0;
  char m_vinStatusText[32] = {};   // 正常提示 / 错误提示 / SAVED
  std::function<void(const char*)> m_vinSubmitHandler;
  std::function<void()> m_vinCancelHandler;
};
```

实现要点：

```cpp
bool LvglDashboard::linkPageVisible() const {
  return m_linkPage != nullptr &&
         !lv_obj_has_flag(m_linkPage, LV_OBJ_FLAG_HIDDEN);
}

void LvglDashboard::vinEditorShow(const char* initialVin) {
  // 只接受 17 位以内；非法输入按空处理
  size_t n = initialVin ? strlen(initialVin) : 0;
  if (n > 17) n = 17;
  memcpy(m_vinEdit, initialVin ? initialVin : "", n);
  m_vinEdit[n] = '\0';
  m_vinEditLen = (uint8_t)n;

  m_vinNumericLayout = false;
  m_vinStatusText[0] = '\0';
  lv_obj_set_style_text_color(m_vinStatus, kDim, 0);
  vinEditorSetLayout(false);
  vinEditorRefresh();
  lv_obj_set_hidden(m_vinEditor, false);
  lv_obj_move_foreground(m_vinEditor);
  m_vinEditorVisible = true;
}

void LvglDashboard::vinEditorHide() {
  m_vinEditorVisible = false;
  if (m_vinEditor) lv_obj_set_hidden(m_vinEditor, true);
  m_vinStatusText[0] = '\0'; // 或恢复默认提示
}

bool LvglDashboard::vinEditorHandleTap(int16_t x, int16_t y) {
  if (!m_vinEditorVisible) return false;

  const int key = vinHitKey(x, y);
  if (key < 0) return true;  // 点在编辑层内但不在按键：消费掉，避免穿透

  if (key == 29) {           // X
    if (m_vinCancelHandler) m_vinCancelHandler();
    return true;
  }
  if (key == 28) {           // OK
    vinEditorSubmitOrError();
    return true;
  }
  if (key == (m_vinNumericLayout ? 10 : 26)) {  // DEL
    if (m_vinEditLen > 0) {
      m_vinEdit[--m_vinEditLen] = '\0';
      vinEditorRefresh();
    }
    return true;
  }
  if (key == (m_vinNumericLayout ? 11 : 27)) {  // 123 / ABC
    m_vinNumericLayout = !m_vinNumericLayout;
    vinEditorSetLayout(m_vinNumericLayout);
    vinEditorRefresh();
    return true;
  }

  // 字符键
  const char ch = m_vinNumericLayout
                      ? (key <= 8 ? char('1' + key) : (key == 9 ? '0' : '\0'))
                      : (key <= 25 ? char('A' + key) : '\0');
  if (ch != '\0' && m_vinEditLen < 17) {
    m_vinEdit[m_vinEditLen++] = ch;
    m_vinEdit[m_vinEditLen] = '\0';
    vinEditorRefresh();
  }
  return true;
}
```

`vinEditorRefresh()`：

- 更新 VIN 文本 label；
- 更新 `17 / 17` 计数；
- 默认状态文字 `ENTER 17 CHARACTERS` 或 `READY`；
- 长度不足时把 OK 键置暗。

`vinEditorSetError()` / `vinEditorShowSaved()`：

```cpp
void LvglDashboard::vinEditorSetError(const char* text) {
  snprintf(m_vinStatusText, sizeof(m_vinStatusText), "%s", text ? text : "");
  updateLabel(m_vinStatus, m_vinStatusText);
  lv_obj_set_style_text_color(m_vinStatus, kRed, 0);
}

void LvglDashboard::vinEditorShowSaved() {
  snprintf(m_vinStatusText, sizeof(m_vinStatusText), "SAVED - RESTARTING...");
  updateLabel(m_vinStatus, m_vinStatusText);
  lv_obj_set_style_text_color(m_vinStatus, kGreen, 0);
}
```

`vinEditorSubmitOrError()`：

```cpp
char normalized[18];
const auto err = vinstore::validate(m_vinEdit); // 或 normalize
if (err != vinstore::Error::None) {
  vinEditorSetError(vinstore::errorText(err));
  return;
}
if (m_vinSubmitHandler) m_vinSubmitHandler(m_vinEdit);
```

> `LvglDashboard` 只负责显示与编辑，不直接写 NVS、不直接重启。
> 保存/重启策略留在 `DashboardApp`，便于测试和替换。

### 5.6 触摸分发改造（`DashboardApp`）

当前触摸流程：`pollTouch()` 在松手时调用 `handleTap()`。
新增状态：

```cpp
uint32_t m_touchStartMs = 0;
bool m_longPressConsumed = false;
uint32_t m_vinRestartAt = 0;
```

`pollTouch()` 规则：

1. **编辑器打开时**：
   - 按下：`m_power.notifyUserActivity()`，记录起点，不做亮度/小精灵手势；
   - 移动：直接返回；
   - 松手：若 `max(|dx|,|dy|) < CFG_VIN_TAP_PX`，调用
     `dashboard.vinEditorHandleTap(m_touchLastX, m_touchLastY)`；
   - 不触发双击截图。
2. **编辑器未打开**：
   - 保留现有亮度滑动与小精灵滑动逻辑；
   - 在按下后的每轮 poll 中检查长按：
     ```cpp
     if (!m_longPressConsumed &&
         m_model.link == UiLinkState::Ready &&
         now - m_touchStartMs >= CFG_VIN_LONG_PRESS_MS &&
         abs(dx) < CFG_VIN_TAP_PX && abs(dy) < CFG_VIN_TAP_PX &&
         m_touchStartY < CFG_VIN_TOP_ZONE_PX) {
       m_longPressConsumed = true;
       openVinEditor();
       return;
     }
     ```
   - 松手时若 `m_longPressConsumed`，只重置标志并返回，不进入 `handleTap()`；
   - 链接页点击 VIN 热区：
     ```cpp
     if (dashboard.linkPageVisible() &&
         x >= 34 && x <= 432 && y >= 344 && y <= 390) {
       openVinEditor();
       return;
     }
     ```
     放在双击截图判断之前。

`openVinEditor()`：

```cpp
void DashboardApp::openVinEditor() {
  if (m_elf.visible()) m_elf.hide();
  m_hasRenderedModel = false;          // 关闭后强制补一帧
  m_power.notifyUserActivity();
  dashboard.vinEditorShow(vinstore::current());
}
```

### 5.7 主循环：暂停与保存

`DashboardApp::loop()`：

```cpp
const bool vinEditing = dashboard.vinEditorVisible();

if (vinEditing) {
  // 编辑期间不睡眠、不跑 BLE、不重绘仪表。
  m_power.notifyUserActivity();
} else if (m_sourceStarted && !elfVisible) {
  m_source.loop();
  m_model = m_source.model();
}

// 保存后的延时重启
if (m_vinRestartAt != 0 &&
    static_cast<int32_t>(millis() - m_vinRestartAt) >= 0) {
  evtlog::flush();
  Serial.flush();
  ESP.restart();
}
```

渲染条件增加 `!vinEditing`：

```cpp
if (!dashboard.bootAnimationRunning() && !m_elf.visible() && !vinEditing &&
    now - m_lastRenderMs >= 50) { ... }
```

初始化时注册回调：

```cpp
dashboard.vinEditorSetSubmitHandler(
    [this](const char* vin) { handleVinSubmit(vin); });
dashboard.vinEditorSetCancelHandler([this]() {
  dashboard.vinEditorHide();
  m_hasRenderedModel = false;
  m_power.notifyUserActivity();
});
```

`handleVinSubmit()`：

```cpp
void DashboardApp::handleVinSubmit(const char* vin) {
  if (strcmp(vin, vinstore::current()) == 0) {
    // 没改动：直接关闭，不写 NVS、不重启。
    dashboard.vinEditorHide();
    m_hasRenderedModel = false;
    return;
  }

  const vinstore::Error err = vinstore::save(vin);
  if (err != vinstore::Error::None) {
    dashboard.vinEditorSetError(vinstore::errorText(err));
    EVT_LOGE("VIN", "save failed err=%u", static_cast<unsigned>(err));
    return;
  }

  // 只记录尾号，避免完整 VIN 落盘/串口。
  EVT_LOGI("VIN", "updated tail=%s", vin + 11);
  dashboard.vinEditorShowSaved();
  m_vinRestartAt = millis() + CFG_VIN_RESTART_DELAY_MS;
}
```

> `vin + 11` 仅在 `validate()` 通过后使用；若改为支持更短 VIN，需要改用
> `vin + max(0, len - 6)`。

---

## 6. 配置项

在 `include/config.h` 的 VIN 段增加：

```c
// 出厂默认 VIN。设备上输入并保存后，NVS 覆盖值优先于这里。
#define CFG_TESLA_VIN "LRWYGCFJ8RC571213"

// 留空 = 由 VIN 推导广播名：S + sha1(VIN) 前 16 位 hex + C
// 注意：非空会覆盖 VIN 推导结果，此时改 VIN 不会改变扫描目标。
#define CFG_TESLA_NAME ""

// 设备端 VIN 输入
#define CFG_VIN_INPUT_ENABLE 1
#define CFG_VIN_LONG_PRESS_MS 1200   // 仪表页长按顶部进入编辑
#define CFG_VIN_TAP_PX 20           // 轻点/长按允许的位移
#define CFG_VIN_TOP_ZONE_PX 70      // 长按入口的顶部区域
#define CFG_VIN_RESTART_DELAY_MS 900
#define CFG_VIN_STRICT_ISO 0        // 1 = 拒绝 I/O/Q
```

- `CFG_VIN_INPUT_ENABLE=0` 时：
  - 不创建编辑器对象；
  - 不响应入口手势；
  - 仍从 NVS 读取 VIN（保留已保存值），行为等价“只读”。
- 不要把上面这些宏放进 `BackendConfig.h` 的默认值列表；它们只属于固件 UI。

---

## 7. 文件改动清单

| 文件 | 改动 |
|---|---|
| `ESP32-S3-ROUND-DISPLAY/include/config.h` | 更新 VIN 注释，新增输入相关配置 |
| `ESP32-S3-ROUND-DISPLAY/include/config/VinStore.h` | **新增** VinStore 接口 |
| `ESP32-S3-ROUND-DISPLAY/src/config/VinStore.cpp` | **新增** NVS 读写、校验、回退 |
| `TeslaBleBackend/src/data/TeslaBleSource.h` | 新增 `setVin()`、`m_vin` |
| `TeslaBleBackend/src/data/TeslaBleSource.cpp` | 使用运行期 VIN 推导广播名、传给 client |
| `TeslaBleBackend/src/tesla/TeslaClient.h` | `begin(BleLink*, const char* vin)`，可选 `setVin()` |
| `TeslaBleBackend/src/tesla/TeslaClient.cpp` | 使用运行期 VIN；日志只打尾号 |
| `ESP32-S3-ROUND-DISPLAY/include/ui/LvglDashboard.h` | 新增 VIN 行、编辑器 API/成员 |
| `ESP32-S3-ROUND-DISPLAY/src/ui/LvglDashboard.cpp` | 链接页 VIN 行、编辑器构建/命中/刷新 |
| `ESP32-S3-ROUND-DISPLAY/include/app/DashboardApp.h` | 新增长按/重启状态、`openVinEditor`、`handleVinSubmit` |
| `ESP32-S3-ROUND-DISPLAY/src/app/DashboardApp.cpp` | 初始化 VinStore、触摸分发、暂停轮询、延时重启 |
| `ESP32-S3-ROUND-DISPLAY/src/data/TeslaUiSource.cpp` | 从 `vinstore::current()` 取 VIN，注入 backend |
| `ESP32-S3-ROUND-DISPLAY/README.md` | 把“改 config.h 重新编译”更新为“设备端输入 / NVS 覆盖” |
| `ESP32-S3-ROUND-DISPLAY/design/VIN-INPUT-DESIGN.md` | 本文档 |

需要同步更新文档中提到 VIN 配置方式的位置：

- `ESP32-S3-ROUND-DISPLAY/README.md` 的配置章节；
- `simulator/docs/ARCHITECTURE.md` 中“VIN 通过 `tesla_backend_user_config.h` 配置”的说明；
- `TESLA-DASHBOARD/开发环境准备.md` 里“两处 VIN 必须一致”的说明，
  补充“圆屏设备可在机器上直接修改”。

---

## 8. 边界情况与处理

| 场景 | 行为 |
|---|---|
| NVS 无值 | 使用 `CFG_TESLA_VIN` |
| NVS 值长度/字符非法 | 忽略并回退默认值，记 WARN，不删除 NVS（方便排查） |
| `CFG_TESLA_VIN` 也非法/为空 | `current()` 返回 `""`；后端退回前缀/UUID 扫描；UI 显示 `VIN NOT SET` |
| 用户只输入 16 位按 OK | 不保存，状态行显示 `VIN MUST BE 17 CHARACTERS` |
| 用户输入超过 17 位 | 忽略多余字符 |
| 用户输入与当前 VIN 相同 | 关闭编辑器，不写 NVS、不重启 |
| NVS 写入失败 | 留在编辑器，显示 `SAVE FAILED`，记录 ERROR |
| 编辑期间自动休眠 | 每轮 `notifyUserActivity()`，禁止进入 deep sleep |
| 编辑期间 BLE 断开/配对提示 | BLE 轮询暂停，底层状态冻结；取消后恢复；保存后重启 |
| 编辑期间 OTA | OTA 覆盖层优先级更高；本期不处理并发，文档标注“升级时不要编辑 VIN” |
| `CFG_TESLA_NAME` 非空 | 日志 WARN：改 VIN 不会改变扫描目标；UI 仍允许保存 |
| 完整 VIN 日志 | 禁止。串口/事件日志只允许后 6 位或 `VIN tail=...` |
| 切到另一台车 | 广播名与握手 VIN 同时变化；若本机 client key 未被新车加入白名单，走既有配钥流程 |
| 断电在 NVS 写入瞬间 | Preferences 自身保证原子性；重启后为旧值或新值之一 |

---

## 9. Phase 2：运行期热切换（可选，不阻塞 Phase 1）

如果用户不接受保存后重启，再实现：

```cpp
// BleLink
void retarget(const String& name);   // 清目标、断开、重新扫描

// TeslaClient
void setVin(const char* vin);        // m_crypto.begin(String(vin)); 清空接收缓冲

// TeslaBleSource
void applyVin(const char* vin);
```

`BleLink::retarget()` 参考实现：

```cpp
void BleLink::retarget(const String& name) {
  m_targetName = name;
  m_hasTarget = false;
  m_targetAddr.clear();
  m_failCount = 0;
  m_scanStarted = false;

  if (NimBLEDevice::getScan() && NimBLEDevice::getScan()->isScanning()) {
    NimBLEDevice::getScan()->stop();
  }
  if (m_client && m_client->isConnected()) {
    m_client->disconnect();
  }
  m_state = State::Scanning;
}
```

风险：

- NimBLE 的 `disconnect()` 与扫描停止是异步的，需确认不会在回调线程里操作；
- 已连接的 GATT/CCCD、发送队列、`m_writeInFlight` 需要完整重置；
- 配钥流程中的 NFC 提示可能残留。

因此 Phase 2 必须单独做端到端测试（真车/模拟器各一轮），不建议和 Phase 1 一起实现。

---

## 10. 备选方案：USB Web Serial 输入（不推荐作为主方案）

现有 `tools/ota/index.html` 已经通过 USB CDC 与设备通信，协议为 ASCII 行。
可以增加：

```text
主机 → 设备：VIN_GET
设备 → 主机：VIN: <17 位>
主机 → 设备：VIN_SET <17 位>
设备 → 主机：VIN: OK / VIN: ERR <code>
```

优点：

- 在电脑上打字，17 位输入体验最好；
- 不需要在 466px 圆屏上做 30 键键盘；
- 复用现有 OTA 页面和 `UsbOta::handleLine()`。

缺点：

- 必须连 USB、打开网页；
- 仍然需要处理“保存后重启/热切换”；
- 用户在没有电脑时无法改 VIN。

建议：Phase 1 先做设备端键盘；如果实际使用中觉得输入麻烦，再补 Web Serial。
两者共用 `VinStore`，互不冲突。

---

## 11. 验证清单

实现完成后按顺序验证（**不要自动编译/烧录，由用户执行**）：

### 11.1 基础回归

- [ ] 不输入 VIN，直接上电：行为与改动前一致，使用 `CFG_TESLA_VIN`。
- [ ] 串口启动日志出现 `[VIN] source=default tail=xxxxxx`。
- [ ] 链接页显示 `VIN LRWY...` 与 `TAP VIN TO CHANGE`。
- [ ] 模拟器使用相同 VIN 启动，设备能正常扫描/连接/握手。

### 11.2 输入流程

- [ ] 链接页点击 VIN 行，编辑器打开，输入框预填当前 VIN。
- [ ] `DEL` 可删除，`123`/`ABC` 可切换键盘，输入上限 17。
- [ ] 长度为 16 时 `OK` 置暗；点击 `OK` 显示 `VIN MUST BE 17 CHARACTERS`。
- [ ] 输入 17 位后 `OK` 高亮；点击后显示 `SAVED - RESTARTING...`。
- [ ] 约 900ms 后设备自动重启。
- [ ] 重启日志出现 `[VIN] source=nvs tail=xxxxxx`。
- [ ] 链接页显示新 VIN；BLE 广播名变为 `S+sha1(newVin)[:16]+C`。
- [ ] 模拟器改同一 VIN 后，设备能连接并完成握手。

### 11.3 持久化与升级

- [ ] 断电重启后仍是新 VIN。
- [ ] USB OTA 升级固件后仍是新 VIN（NVS 不被擦除）。
- [ ] 手动擦除 NVS/恢复出厂后回到 `CFG_TESLA_VIN`。

### 11.4 手势与并发

- [ ] 编辑器打开时，上下滑不会调亮度。
- [ ] 编辑器打开时，左右滑不会呼出小精灵。
- [ ] 编辑器打开时双击不会触发串口截图。
- [ ] 仪表页长按顶部 1.2s 打开编辑器；短按不会误触。
- [ ] 长按打开后松手，不会再次触发截图或点击。
- [ ] 编辑期间放置 3 分钟不会自动休眠。
- [ ] 取消后仪表恢复刷新，BLE 正常继续。

### 11.5 异常

- [ ] 输入含 `I/O/Q` 时（`CFG_VIN_STRICT_ISO=1`）被拒绝并提示。
- [ ] NVS 写入失败时留在编辑器并提示（可临时改代码模拟）。
- [ ] `CFG_TESLA_VIN` 置空时 UI 显示 `VIN NOT SET`，后端日志说明退回前缀扫描。
- [ ] 反复打开/关闭编辑器 20 次，`ESP.getFreeHeap()` 无明显下降。

### 11.6 隐私

- [ ] 串口日志不出现完整 VIN。
- [ ] 事件日志 `LOG_LIST/LOG_READ` 中不出现完整 VIN。

---

## 12. 给实现 AI 的执行顺序

严格按以下顺序改，每步可独立验证：

1. **VinStore**
   - 新建 `include/config/VinStore.h`、`src/config/VinStore.cpp`；
   - 先只实现 `begin/current/validate/normalize/save/clear/errorText`；
   - 在 `DashboardApp::begin()` 中调用 `vinstore::begin()`，串口打印 source/tail。
2. **后端运行期 VIN**
   - 改 `TeslaBleSource::setVin()`、`TeslaBleSource::begin()`；
   - 改 `TeslaClient::begin(BleLink*, const char* vin)`；
   - 改 `TeslaUiSource::begin()` 注入 `vinstore::current()`；
   - 此时默认 VIN 行为应完全不变（回归点）。
3. **链接页 VIN 行**
   - `buildLinkPage()` 增加 label；
   - `render()` 填充 `model.vin`；
   - 增加 `linkPageVisible()`。
4. **VIN 编辑器（纯 UI）**
   - `buildVinEditor()`、30 个按键、字母/数字布局、刷新函数；
   - `vinEditorShow/Hide/HandleTap`；
   - 先用串口打印点击结果验证命中坐标。
5. **接入 DashboardApp**
   - 双击截图之前处理 VIN 热区；
   - 编辑器打开时中断手势与 BLE 轮询；
   - 实现 `openVinEditor()`、取消回调。
6. **保存与重启**
   - 注册 submit handler；
   - `vinstore::save()` + `SAVED` 提示 + 延时 `ESP.restart()`；
   - 错误提示与日志脱敏。
7. **长按入口**
   - 增加 `m_touchStartMs`、`m_longPressConsumed`；
   - 仪表页顶部 1.2s 长按打开；
   - 确认不与亮度/小精灵手势冲突。
8. **文档与回归**
   - 更新 README/ARCHITECTURE；
   - 按 §11 清单逐项验证。

### 12.1 编码约定提醒

- 不自动运行 `pio run`，不修改 `platformio.ini`（见 `AGENTS.md`）。
- 保持现有的匿名 namespace 常量、`makeLabel`/`makePanel` 风格。
- 新增日志沿用 `CFG_LOG` / `EVT_LOGI`。
- LVGL 对象只在 `begin()` 创建一次，`show/hide` 不重建。
- 所有触摸坐标都是 **LVGL 逻辑坐标**（`display.mapTouchToScreen()` 之后）。
- 编辑器对象必须 `lv_obj_move_foreground()`，并确保在 Boot 动画层之上创建。

---

## 13. 验收标准（Definition of Done）

1. 不修改代码即可在设备上把 VIN 改成任意 17 位合法字符串。
2. 保存后重启，新的 VIN 同时作用于：
   - BLE 扫描广播名；
   - 加密握手元数据；
   - 链接页展示。
3. VIN 在断电、重启、OTA 升级后保持。
4. 取消编辑不产生任何副作用。
5. 输入错误有明确提示，不会保存非法 VIN。
6. 编辑期间不会误触亮度/小精灵/截图/自动休眠。
7. 完整 VIN 不出现在串口日志和事件日志中。
8. 默认未配置 NVS 时，固件行为与改动前完全一致。
