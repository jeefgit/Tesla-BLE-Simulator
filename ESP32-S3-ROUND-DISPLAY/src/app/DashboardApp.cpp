#include "app/DashboardApp.h"

#include <Arduino.h>
#include <Wire.h>
#include <esp_system.h>
#include <string.h>

#include "config.h"
#include "config/ConfigMode.h"
#include "config/VinStore.h"
#include "display/RoundDisplay.h"
#include "display/ScreenShot.h"
#include "input/Cst9217Touch.h"
#include "input/Qmi8658.h"
#include "log/EventLog.h"
#include "platform/boards/waveshare_esp32_s3_touch_amoled_175_b.h"
#include "power/PowerManager.h"
#include "ui/LvglDashboard.h"

namespace {

// 车速数字/弧线的过渡时长。重绘节流是 50ms，原来 30ms 会让过渡在下一帧之前
// 就结束，肉眼看到的是 20 → 17 的瞬间跳变；400ms 约跨 8 帧，数值逐步逼近目标。
constexpr uint32_t kSpeedTransitionMs = 400;

// BOOT 键接 GPIO0，板上有外部上拉，按下为低电平。
constexpr uint8_t kBootButtonPin = 0;
constexpr uint32_t kBootDebounceMs = 30;
constexpr uint32_t kBootLongPressMs = 2000;  // 长按切换配置模式

// PWR 键不在 ESP32 的 GPIO 上，而是经板载 TCA9554（I2C 0x20）的
// EXIO4 读取：高电平表示按下，低电平表示松开。
constexpr uint8_t kPwrButtonSda = 15;
constexpr uint8_t kPwrButtonScl = 14;
constexpr uint8_t kPwrExpanderAddress = 0x20;
constexpr uint8_t kPwrExpanderInputReg = 0x00;
constexpr uint8_t kPwrExpanderConfigReg = 0x03;
constexpr uint8_t kPwrButtonMask = 1u << 4;  // EXIO4
constexpr uint32_t kPwrDebounceMs = 30;
constexpr uint32_t kPwrLongPressMs = 3000;

// 上下滑动调亮度的手势参数，全部按 LVGL 画面坐标计算（已换算旋转）。
// 纵向累计位移超过 kSwipeActivatePx、且明显大于横向位移才判定为滑动；
// 激活后每像素对应 1 档亮度（实际范围由 RoundDisplay 限制在 10%~80%），
// 上滑调亮、下滑调暗。
constexpr int32_t kSwipeActivatePx = 24;
constexpr int32_t kSwipeDominancePx = 16;
constexpr int32_t kBrightnessStepPerPx = 1;

int16_t readExpanderReg(uint8_t reg) {
  Wire.beginTransmission(kPwrExpanderAddress);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return -1;
  }
  if (Wire.requestFrom(kPwrExpanderAddress, 1) != 1) {
    return -1;
  }
  return static_cast<int16_t>(Wire.read());
}

bool writeExpanderReg(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(kPwrExpanderAddress);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

// 事件日志：堆采样 5 秒一次；低水位告警与恢复之间留 8 KB 滞回。
constexpr uint32_t kHeapSampleMs = 5000;
constexpr uint32_t kHeapRecoverMarginBytes = 8192;

// 异常复位时把 ESP-IDF 的原因码转成短字符串，便于日志直接阅读。
const char* resetReasonName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:
      return "POWERON";
    case ESP_RST_EXT:
      return "EXT";
    case ESP_RST_SW:
      return "SW";
    case ESP_RST_PANIC:
      return "PANIC";
    case ESP_RST_INT_WDT:
      return "INT_WDT";
    case ESP_RST_TASK_WDT:
      return "TASK_WDT";
    case ESP_RST_WDT:
      return "WDT";
    case ESP_RST_DEEPSLEEP:
      return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:
      return "BROWNOUT";
    case ESP_RST_SDIO:
      return "SDIO";
    default:
      return "UNKNOWN";
  }
}

RoundDisplay display;
LvglDashboard dashboard;

bool sameModel(const UiModel& a, const UiModel& b) {
  return a.link == b.link && a.pairing == b.pairing &&
         a.locked == b.locked && a.charging == b.charging &&
         a.powerValid == b.powerValid &&
         a.chargePowerValid == b.chargePowerValid &&
         a.socValid == b.socValid && a.rangeValid == b.rangeValid &&
         a.temperatureValid == b.temperatureValid &&
         a.doorsValid == b.doorsValid &&
         memcmp(a.doors, b.doors, sizeof(a.doors)) == 0 &&
         a.motorPowerKw == b.motorPowerKw &&
         a.chargePowerKw == b.chargePowerKw && a.soc == b.soc &&
         a.rangeKm == b.rangeKm && a.insideTempC == b.insideTempC &&
         a.outsideTempC == b.outsideTempC && a.gear == b.gear &&
         a.pairingRemainMs == b.pairingRemainMs &&
         a.pairingAttempt == b.pairingAttempt &&
         a.pairingMaxAttempts == b.pairingMaxAttempts &&
         a.elapsedSeconds == b.elapsedSeconds &&
         strcmp(a.vin, b.vin) == 0;
}

// 日志/事件只允许出现 VIN 后 6 位：长度不足时返回原串（不越界）。
const char* vinTail(const char* vin) {
  if (vin == nullptr) {
    return "";
  }
  const size_t len = strlen(vin);
  return len > 6 ? vin + (len - 6) : vin;
}

}  // namespace

void DashboardApp::begin() {
  // 事件日志最优先初始化：后续初始化失败/复位原因都要能落盘。
  evtlog::begin();
  // VIN 尽早加载：后续 UI 与 BLE 后端都读同一个生效值（NVS 优先）。
  vinstore::begin();
  // 配置模式标志也尽早读：决定本次是跑 BLE 仪表还是只开配置热点。
  configmode::begin();
  m_configMode = configmode::active();
  const esp_reset_reason_t resetReason = esp_reset_reason();
  if (resetReason != ESP_RST_POWERON && resetReason != ESP_RST_SW &&
      resetReason != ESP_RST_DEEPSLEEP) {
    EVT_LOGE("RESET", "reason=%s", resetReasonName(resetReason));
  }
  // 先读唤醒原因，供休眠/唤醒确认逻辑使用。
  m_power.detectWakeReason();
  if (!display.begin()) {
    Serial.println("Display initialization failed.");
    EVT_LOGE("DISP", "initialization failed");
    return;
  }
  Serial.printf("Display ready, free heap: %u bytes\n", ESP.getFreeHeap());
  dashboard.begin();
  Serial.printf("UI built, free heap: %u bytes\n", ESP.getFreeHeap());
  // VIN 编辑器取消（X 键）：只关闭并强制补一帧，不写 NVS。
  dashboard.vinEditorSetCancelHandler([this]() {
    dashboard.vinEditorHide();
    m_hasRenderedModel = false;
    m_power.notifyUserActivity();
  });
  // VIN 编辑器提交（OK 键）：校验已在 UI 侧完成，这里负责写 NVS 与延时重启。
  dashboard.vinEditorSetSubmitHandler(
      [this](const char* vin) { handleVinSubmit(vin); });
  // 浮层必须在仪表和开机动画之后建，才能盖在所有页面之上。
  m_elf.begin();
#if !CFG_DEMO_MODE
  m_sourceStarted = false;
  m_model.link = UiLinkState::Scanning;
#endif
  // 配置模式：这里就把热点开好，第一帧配置页才能显示真实的 SSID / IP。
  // （之前在 loop() 里开热点，配置页只会一直显示扫描画面。）
  if (m_configMode) {
    m_web.begin();
    m_webStarted = true;
    dashboard.setConfigMode(true);
    dashboard.setConfigInfo(m_web.ssid(), m_web.ip());
  }
  dashboard.render(m_model);
  Serial.printf("First frame, free heap: %u bytes\n", ESP.getFreeHeap());
  m_lastRenderedModel = m_model;
  m_hasRenderedModel = true;
  m_initialized = true;

  // BOOT 键平时是高电平，短按一次把画面顺时针旋转 90°。
  pinMode(kBootButtonPin, INPUT_PULLUP);
  beginPowerButton();
  beginTouch();
  m_power.begin(&m_imu, &display);
  m_ota.begin(&display);
  // 网页配置热点只在配置模式开启（见上面 m_configMode 分支），与 BLE 互斥，
  // 避免内存冲突。

#if CFG_ELF_SOUND_ENABLE
  // 麦克风放在最后：ES7210 挂在 15/14 的 Wire 总线上，必须等
  // beginPowerButton() 的 Wire.begin() 之后才能初始化。
  m_soundReady = m_sound.begin();
#endif
}

void DashboardApp::beginTouch() {
  const BoardProfile& board = kWaveshareEsp32S3TouchAmoled175B;
  // PWR 键的 TCA9554 已经把 15/14 上的 Wire 起好了，触摸芯片是同一总线的
  // 另一个设备（0x5A），复用这个实例即可。
  if (!m_touch.begin(Wire, board.touchAddress, board.pinTouchReset,
                     board.pinTouchInt)) {
    CFG_LOG("[TOUCH] CST9217 not found; swipe brightness disabled");
    EVT_LOGE("TOUCH", "CST9217 not found");
  }
}

void DashboardApp::beginPowerButton() {
  Wire.begin(kPwrButtonSda, kPwrButtonScl);
  Wire.setClock(100000);

  // QMI8658 与 TCA9554 共用 15/14，必须在 Wire.begin 之后初始化。
  m_imu.begin(Wire, 0x6B);

  const int16_t config = readExpanderReg(kPwrExpanderConfigReg);
  if (config < 0) {
    CFG_LOG("[BTN] TCA9554 not found; PWR long-press disabled");
    EVT_LOGE("IO", "TCA9554 not found");
    return;
  }
  // 配置寄存器里 1 = 输入。EXIO4 必须保持输入才能读回 PWR 状态。
  if ((config & kPwrButtonMask) == 0) {
    if (!writeExpanderReg(kPwrExpanderConfigReg,
                          static_cast<uint8_t>(config | kPwrButtonMask))) {
      CFG_LOG("[BTN] failed to configure TCA9554 EXIO4 as input");
      EVT_LOGE("IO", "TCA9554 EXIO4 config failed");
      return;
    }
  }

  m_pwrAvailable = true;
  const int16_t inputs = readExpanderReg(kPwrExpanderInputReg);
  m_pwrRawPressed = inputs >= 0 && (inputs & kPwrButtonMask) != 0;
  m_pwrPressed = m_pwrRawPressed;
  // 如果开机时 PWR 仍被按住（例如正用它上电），先等松开再进入长按检测，
  // 避免上电动作被立刻当成重启。
  m_pwrArmed = !m_pwrRawPressed;
  CFG_LOG("[BTN] PWR button ready (EXIO4, %s)",
          m_pwrRawPressed ? "held" : "released");
}

void DashboardApp::pollPowerButton() {
  if (!m_pwrAvailable) {
    return;
  }
  const int16_t inputs = readExpanderReg(kPwrExpanderInputReg);
  if (inputs < 0) {
    // 总线偶发错误时保持上一次状态，避免误判按下/松开。
    return;
  }
  const bool rawPressed = (inputs & kPwrButtonMask) != 0;
  const uint32_t now = millis();
  if (rawPressed != m_pwrRawPressed) {
    m_pwrRawPressed = rawPressed;
    m_pwrLastChangeMs = now;
    return;
  }
  if (now - m_pwrLastChangeMs < kPwrDebounceMs) {
    return;
  }
  if (rawPressed != m_pwrPressed) {
    m_pwrPressed = rawPressed;
    if (m_pwrPressed) {
      m_pwrPressStartMs = now;
      m_power.notifyUserActivity();
      CFG_LOG("[BTN] PWR pressed (hold %ums to restart)",
              static_cast<unsigned>(kPwrLongPressMs));
    } else {
      // 松开后才允许下一次长按；这也让开机时按住的上电动作失效。
      m_pwrArmed = true;
      m_power.notifyUserActivity();
      CFG_LOG("[BTN] PWR released");
    }
  }
  if (m_pwrPressed && m_pwrArmed &&
      now - m_pwrPressStartMs >= kPwrLongPressMs) {
    CFG_LOG("[BTN] PWR long press -> restart");
    EVT_LOGW("BTN", "restart by user hold=%ums",
             static_cast<unsigned>(kPwrLongPressMs));
    // 重启前把最后一条日志完整落盘，否则重启后就看不到了。
    evtlog::flush();
    Serial.flush();
    ESP.restart();
  }
}

void DashboardApp::pollBootButton() {
  const bool rawPressed = digitalRead(kBootButtonPin) == LOW;
  const uint32_t now = millis();
  if (rawPressed != m_bootRawPressed) {
    m_bootRawPressed = rawPressed;
    m_bootLastChangeMs = now;
    return;
  }
  // 电平稳定 kBootDebounceMs 后才算一次有效按下，避免抖动。
  if (rawPressed != m_bootPressed &&
      now - m_bootLastChangeMs >= kBootDebounceMs) {
    m_bootPressed = rawPressed;
    if (m_bootPressed) {
      m_bootPressStartMs = now;
      m_bootLongPressFired = false;
      m_power.notifyUserActivity();
    } else if (!m_bootLongPressFired) {
      // 松手且未触发长按 -> 短按，旋转画面。
      display.rotateClockwise();
    }
  }
  // 长按 BOOT：切换配置模式（写 NVS 后重启生效）。两种模式互斥，
  // 重启后 BLE 与 WiFi AP 只会启动其中一个，避免内存冲突。
  if (m_bootPressed && !m_bootLongPressFired &&
      now - m_bootPressStartMs >= kBootLongPressMs) {
    m_bootLongPressFired = true;
    if (m_configMode) {
      configmode::exit();
      CFG_LOG("[BTN] BOOT long press -> exit config mode, restart");
      EVT_LOGW("BTN", "exit config mode by hold");
    } else {
      configmode::enter();
      CFG_LOG("[BTN] BOOT long press -> enter config mode, restart");
      EVT_LOGW("BTN", "enter config mode by hold");
    }
    evtlog::flush();
    Serial.flush();
    ESP.restart();
  }
}

void DashboardApp::pollTouch() {
  int16_t rawX = 0;
  int16_t rawY = 0;
  const bool pressed = m_touch.poll(rawX, rawY);
  // VIN 编辑器打开时独占触摸：只处理按键点击，不做亮度/小精灵/截图手势。
  const bool vinEditing = dashboard.vinEditorVisible();
  if (!pressed) {
    if (m_touchDown) {
      m_touchDown = false;
      m_touchHorizontal = false;
      // 长按已经打开 VIN 编辑器：松手只清理手势状态，绝不触发截图/轻点/按键。
      if (m_longPressConsumed) {
        m_longPressConsumed = false;
        m_swipeActive = false;
        m_elfSwipeLatched = false;
        return;
      }
      // 编辑器按键已在按下时处理：松手只清理状态，避免 X 取消后又被当成
      // 链接页点击而重新打开编辑器。
      if (m_vinTapHandled) {
        m_vinTapHandled = false;
        m_swipeActive = false;
        m_elfSwipeLatched = false;
        return;
      }
      // 小精灵可见时：松手 600ms 后视线回到随机游走；几乎没移动则算轻点。
      const int32_t travel =
          max(abs(m_touchLastX - m_touchStartX),
              abs(m_touchLastY - m_touchStartY));
      if (vinEditing) {
        // 按键已在按下时处理；这里只兜底处理漏掉按下事件的轻点。
        if (!m_vinTapHandled && travel < CFG_VIN_TAP_PX) {
          dashboard.vinEditorHandleTap(m_touchLastX, m_touchLastY);
        }
        m_vinTapHandled = false;
        m_swipeActive = false;
        m_elfSwipeLatched = false;
        return;
      }
      if (m_elf.visible()) {
        m_elf.touchEnd();
        if (travel < CFG_ELF_TAP_PX) {
          m_elf.poke(m_touchLastX, m_touchLastY);
        }
      }
      // 轻点第二下触发串口截图（第一下只记时间）。位移超过容差的手势
      // 不算轻点，避免滑动调亮度/呼出小精灵时误触发。
      if (travel < CFG_SCREENSHOT_TAP_PX) {
        handleTap(m_touchLastX, m_touchLastY);
      }
      if (m_swipeActive) {
        m_swipeActive = false;
        // 拖动过程中只写面板寄存器，松手才把最终亮度存进 NVS。
        display.saveBrightness();
      }
    }
    return;
  }

  // 触摸是用户活动，重置自动休眠的静止计时。
  m_power.notifyUserActivity();
  // 触摸原始坐标跟画面旋转走：先换算成 LVGL 逻辑坐标，这样"屏幕上的上下"
  // 在任何旋转角度下都指向同一个手势方向，方向感与看到的画面一致。
  display.mapTouchToScreen(rawX, rawY);
  m_touchLastX = rawX;
  m_touchLastY = rawY;

  if (!m_touchDown) {
    m_touchDown = true;
    m_touchHorizontal = false;
    m_swipeActive = false;
    m_elfSwipeLatched = false;
    m_touchStartX = rawX;
    m_touchStartY = rawY;
    m_touchStartMs = millis();
    m_longPressConsumed = false;
    m_vinTapHandled = false;
    // 编辑器打开时：按下立即处理按键并 return，不呼小精灵、不启动亮度手势。
    if (vinEditing) {
      dashboard.vinEditorHandleTap(rawX, rawY);
      m_vinTapHandled = true;
      return;
    }
    // 链接页 VIN 行：按下即打开，避免快速轻点在松手判定中被漏掉。
    if (!m_elf.visible() && dashboard.linkPageVisible() && rawX >= 20 &&
        rawX <= 446 && rawY >= 330 && rawY <= 405) {
      openVinEditor();
      // 这次按下已经用于打开编辑器，松手不要再去命中键盘按键。
      m_vinTapHandled = true;
      return;
    }
    // 小精灵可见时，按下也算一次交互（并让眼睛立刻看向按下的位置）。
    if (m_elf.visible()) {
      m_elf.lookAt(rawX, rawY);
    }
    return;
  }

  // 编辑器打开时：移动只算用户活动，松手才在上一分支做按键命中。
  if (vinEditing) {
    return;
  }

  const int32_t dx = rawX - m_touchStartX;
  const int32_t dy = rawY - m_touchStartY;
  // 小精灵可见时，手指移动 → 眼睛追随；不影响亮度手势。
  if (m_elf.visible()) {
    m_elf.lookAt(rawX, rawY);
  }
#if CFG_VIN_INPUT_ENABLE
  // 仪表页顶部静止长按 → VIN 编辑器。位移一旦超过容差就交回原有手势，
  // 因此不会和亮度滑动（纵向 24px）/小精灵横滑（60px）互相抢占。
  const uint32_t now = millis();
  if (!m_longPressConsumed && m_model.link == UiLinkState::Ready &&
      now - m_touchStartMs >= CFG_VIN_LONG_PRESS_MS &&
      abs(dx) < CFG_VIN_TAP_PX && abs(dy) < CFG_VIN_TAP_PX &&
      m_touchStartY < CFG_VIN_TOP_ZONE_PX) {
    m_longPressConsumed = true;
    openVinEditor();
    return;
  }
#endif
  // 已进入亮度手势：跟手调节亮度。
  if (m_swipeActive) {
    // 上滑（画面坐标 y 减小）调亮，下滑调暗；亮度实时跟手变化。
    const int32_t delta = (m_swipeAnchorY - rawY) * kBrightnessStepPerPx;
    int32_t target = m_swipeBaseBrightness + delta;
    if (target < 0) {
      target = 0;
    } else if (target > 255) {
      target = 255;
    }
    display.setBrightness(static_cast<uint8_t>(target));
    return;
  }

  // 纵向领先 → 亮度手势。
  if (!m_touchHorizontal && abs(dy) >= kSwipeActivatePx &&
      abs(dy) > abs(dx) + kSwipeDominancePx) {
    m_swipeActive = true;
    m_swipeAnchorY = rawY;
    m_swipeBaseBrightness = display.brightness();
    return;
  }

  // 横向领先 → 小精灵显隐。
  // 注意：判定为横向后仍要继续累计 dx；以前在这里直接 return，导致慢速
  // 滑动永远到不了 60px 阈值，表现为“不灵敏、经常无效”。
  if (!m_touchHorizontal && abs(dx) > abs(dy) + kSwipeDominancePx) {
    m_touchHorizontal = true;
  }
  if (!m_touchHorizontal || m_elfSwipeLatched) {
    return;
  }

  // 方向不敏感：隐藏时左右滑都能呼出，显示时左右滑都能退出。
  // 这样不受触摸 X 轴与画面方向是否镜像的影响。
  if (abs(dx) >= CFG_ELF_SWIPE_PX) {
    if (!m_elf.visible()) {
      if (dashboard.bootAnimationRunning()) {
        return;
      }
      m_elf.show();
    } else {
      m_elf.hide();
      // 浮层关闭后强制下一轮重画一帧，避免显示期间的旧数据。
      m_hasRenderedModel = false;
    }
    m_elfSwipeLatched = true;
  }
}

void DashboardApp::handleTap(int16_t x, int16_t y) {
  CFG_LOG("[VIN] tap x=%d y=%d linkPage=%d", x, y,
          dashboard.linkPageVisible() ? 1 : 0);
  // 链接页 VIN 行热区（LVGL 逻辑坐标，见设计 5.2）：优先于双击截图判定。
  // 比可见文字略大，容忍手指落点偏差。
  if (dashboard.linkPageVisible() && x >= 20 && x <= 446 && y >= 330 &&
      y <= 405) {
    CFG_LOG("[VIN] link VIN row hit");
    openVinEditor();
    return;
  }
  const uint32_t now = millis();
  const bool inWindow =
      m_lastTapValid && (now - m_lastTapMs) <= CFG_SCREENSHOT_DOUBLE_TAP_MS &&
      abs(x - m_lastTapX) <= CFG_SCREENSHOT_TAP_PX &&
      abs(y - m_lastTapY) <= CFG_SCREENSHOT_TAP_PX;
  if (!inWindow) {
    m_lastTapValid = true;
    m_lastTapMs = now;
    m_lastTapX = x;
    m_lastTapY = y;
    return;
  }

  // 双击成立：消费掉这次轻点，避免三连击再触发第二次。
  m_lastTapValid = false;
  screenshot::dumpToSerial(display.rotation());
}

void DashboardApp::openVinEditor() {
#if !CFG_VIN_INPUT_ENABLE
  // 设备端输入关闭：不创建编辑器对象，链接页热区与长按入口都直接失效。
  return;
#else
  // Boot 动画层盖住整个屏幕，期间的点击都是盲点，不打开编辑器。
  if (dashboard.bootAnimationRunning()) {
    return;
  }
  if (m_elf.visible()) {
    m_elf.hide();
  }
  // 关闭后靠这一标志强制补画一帧，避免显示编辑器期间的旧数据。
  m_hasRenderedModel = false;
  m_power.notifyUserActivity();
  dashboard.vinEditorShow(vinstore::current());
  CFG_LOG("[VIN] editor opened");
#endif
}

void DashboardApp::handleVinSubmit(const char* vin) {
  // 已安排重启：忽略重复提交，避免再次写 NVS/重置计时。
  if (m_vinRestartAt != 0) {
    return;
  }
  if (vin == nullptr) {
    return;
  }
  // 没改动：直接关闭，不写 NVS、不重启。
  if (strcmp(vin, vinstore::current()) == 0) {
    dashboard.vinEditorHide();
    m_hasRenderedModel = false;
    m_power.notifyUserActivity();
    return;
  }

  const vinstore::Error err = vinstore::save(vin);
  if (err != vinstore::Error::None) {
    // 留在编辑器里显示错误，重启计时不启动。
    dashboard.vinEditorSetError(vinstore::errorText(err));
    EVT_LOGE("VIN", "save failed err=%u", static_cast<unsigned>(err));
    return;
  }

  // 只记录尾号，禁止完整 VIN 进串口/事件日志。
  EVT_LOGI("VIN", "updated tail=%s", vinTail(vin));
  dashboard.vinEditorShowSaved();
  m_vinRestartAt = millis() + CFG_VIN_RESTART_DELAY_MS;
}

void DashboardApp::setModel(const UiModel& model) { m_model = model; }

void DashboardApp::loop() {
  // 事件日志先跑：即使显示初始化失败停在下面的分支，缓冲也能落盘。
  evtlog::loop();
  if (!m_initialized) {
    delay(1000);
    return;
  }
  // VIN 保存后的延时重启：放在初始化检查之后，确保每轮必达且只触发一次。
  // 重启前编辑器保持可见、BLE 轮询仍暂停，先把事件日志完整落盘。
  if (m_vinRestartAt != 0 &&
      static_cast<int32_t>(millis() - m_vinRestartAt) >= 0) {
    evtlog::flush();
    Serial.flush();
    ESP.restart();
  }
  pollBootButton();
  pollPowerButton();
  pollTouch();
  // USB OTA：收到主机命令说明有人正在操作设备，顺带重置自动休眠计时，
  // 避免网页还连着、设备却先进入 deep sleep。OTA_BEGIN 会在 loop() 内
  // 阻塞到升级结束（成功则直接重启）。
  if (m_ota.loop()) {
    m_power.notifyUserActivity();
  }
  // 网页配置：有手机连上热点 / 正在请求时重置自动休眠计时。
  if (m_web.loop()) {
    m_power.notifyUserActivity();
  }
  // 小精灵可能在内部计时器里自动隐藏：检测可见性下降沿，强制补画一帧。
  // （自动隐藏默认关闭，这里也覆盖横滑退出后的补画。）
  const bool elfVisible = m_elf.visible();
  if (m_elfWasVisible && !elfVisible) {
    m_hasRenderedModel = false;
  }
  m_elfWasVisible = elfVisible;
  // VIN 编辑期间暂停 BLE 轮询/数据刷新与仪表重绘，每轮 notify 防自动休眠。
  const bool vinEditing = dashboard.vinEditorVisible();
  if (vinEditing) {
    m_power.notifyUserActivity();
  }
#if !CFG_DEMO_MODE
  if (!vinEditing && !m_sourceStarted && !dashboard.bootAnimationRunning() &&
      !m_configMode) {
    m_source.begin();
    m_sourceStarted = true;
  }
  if (!vinEditing && m_sourceStarted && !elfVisible) {
    // 小精灵显示期间完全停止 BLE 轮询：不再获取驾驶数据，小精灵只跑本地动画。
    // 隐藏（含 Hiding 动画结束）后恢复轮询，并靠 m_hasRenderedModel=false
    // 强制补画一帧最新数据。
    m_source.loop();
    m_model = m_source.model();
  }
#endif
  // 配置模式：只开 WiFi 热点（BLE 不启动）并保持常醒；正常/演示模式不开热点。
  if (m_configMode) {
    m_power.notifyUserActivity();
    if (!m_webStarted) {
      m_web.begin();
      m_webStarted = true;
    }
  }
  // 车辆状态只在仪表模式同步给浮层（阶段 4 的充电打哈欠/车速瞳孔/急减速眯眼）。
  // 小精灵显示期间不更新：m_model 已冻结，避免用陈旧数据继续驱动联动。
  // VIN 编辑期间同样冻结（m_model 不刷新）。
  if (!elfVisible && !vinEditing) {
    m_elf.setVehicleState(m_model.speedKmh, m_model.speedValid, m_model.charging);
  }
#if CFG_ELF_SOUND_ENABLE
  // 初始化失败时 SoundSensor 内部按 5s 间隔最多重试 5 次。
  if (!m_soundReady) {
    m_soundReady = m_sound.begin();
  }
  // 声音采集只跟随可见性：上升沿启动一次，可见期间每轮读一帧快照
  // （read() 只拷贝，不清 beat 之外的状态，不阻塞 BLE 轮询），
  // 下降沿立即停 I2S 并喂空帧，让浮层马上回到 Off/静音视觉。
  if (elfVisible) {
    if (!m_soundStarted && m_soundReady && !m_soundStartFailed) {
      m_soundStarted = m_sound.start();
      m_soundStartFailed = !m_soundStarted;
    }
    if (m_soundStarted) {
      m_elf.setSoundFrame(m_sound.read());
    }
  } else {
    if (m_soundStarted) {
      m_sound.stop();
      m_soundStarted = false;
      m_elf.setSoundFrame(SoundFrame{});
    }
    // 下次呼出重新尝试启动（例如上次 DMA 内存临时不足）。
    m_soundStartFailed = false;
  }
#endif
  // 静止自动休眠：IMU 运动检测、触摸/按键活动与配钥/充电条件都在这里判断。
  m_power.update(m_model, dashboard.bootAnimationRunning(), m_model.charging);
  const uint32_t now = millis();
  // The boot overlay hides everything, so redrawing the dashboard for its
  // duration would only compete with the ring animation and the BLE stack.
  // 小精灵可见时同样暂停仪表重绘（数据照常更新，退出时补一帧）；
  // VIN 编辑期间也不重绘，避免旧数据盖到编辑层下方。
  if (!dashboard.bootAnimationRunning() && !m_elf.visible() && !vinEditing &&
      now - m_lastRenderMs >= 50) {
    const float speedInputKmh =
        m_model.speedValid ? m_model.speedKmh : 0.0f;
    if (!m_speedInitialized) {
      m_speedInitialized = true;
      m_speedInputKmh = speedInputKmh;
      m_displaySpeedKmh = speedInputKmh;
    } else if (speedInputKmh != m_speedInputKmh) {
      m_speedInputKmh = speedInputKmh;
      if (!m_model.speedValid || !m_speedWasValid) {
        // 丢速或刚恢复有效速度：直接贴到目标值，不要从 0 慢慢扫过去。
        m_displaySpeedKmh = speedInputKmh;
        m_speedFromKmh = speedInputKmh;
      } else {
        // 从当前显示值出发，避免连续更新时回跳。
        m_speedFromKmh = m_displaySpeedKmh;
        m_speedTransitionAt = now;
      }
    }
    m_speedWasValid = m_model.speedValid;

    UiModel renderModel = m_model;
    if (m_speedInitialized) {
      const uint32_t elapsed = now - m_speedTransitionAt;
      const float t = elapsed >= kSpeedTransitionMs
                          ? 1.0f
                          : static_cast<float>(elapsed) / kSpeedTransitionMs;
      const float eased = t * t * (3.0f - 2.0f * t);
      m_displaySpeedKmh =
          m_speedFromKmh + (m_speedInputKmh - m_speedFromKmh) * eased;
      renderModel.speedKmh = m_displaySpeedKmh;
    }

    if (!m_hasRenderedModel ||
        !sameModel(renderModel, m_lastRenderedModel)) {
      dashboard.render(renderModel);
      m_lastRenderedModel = renderModel;
      m_hasRenderedModel = true;
    }
    dashboard.updateSpeedLabel(m_displaySpeedKmh, m_model.speedValid);
    m_lastRenderMs = now;
  }
  // 堆采样放在主循环末尾的低频位置：5 秒一次，只在越过阈值时记录。
  const uint32_t heapNow = millis();
  if (heapNow - m_lastHeapSampleMs >= kHeapSampleMs) {
    m_lastHeapSampleMs = heapNow;
    const uint32_t freeHeap = ESP.getFreeHeap();
    const uint32_t minHeap = ESP.getMinFreeHeap();
    const uint32_t largestHeap = ESP.getMaxAllocHeap();
    if (!m_heapLow && freeHeap < CFG_EVTLOG_HEAP_WARN_BYTES) {
      m_heapLow = true;
      EVT_LOGW("HEAP", "free=%u min=%u largest=%u", freeHeap, minHeap,
               largestHeap);
    } else if (m_heapLow &&
               freeHeap > CFG_EVTLOG_HEAP_WARN_BYTES + kHeapRecoverMarginBytes) {
      m_heapLow = false;
      EVT_LOGI("HEAP", "recovered free=%u min=%u largest=%u", freeHeap,
               minHeap, largestHeap);
    }
  }
  display.loop();
}
