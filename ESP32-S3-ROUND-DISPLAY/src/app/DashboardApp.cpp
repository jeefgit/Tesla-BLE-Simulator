#include "app/DashboardApp.h"

#include <Arduino.h>
#include <Wire.h>
#include <string.h>

#include "config.h"
#include "display/RoundDisplay.h"
#include "ui/LvglDashboard.h"

namespace {

constexpr uint32_t kSpeedTransitionMs = 30;

// BOOT 键接 GPIO0，板上有外部上拉，按下为低电平。
constexpr uint8_t kBootButtonPin = 0;
constexpr uint32_t kBootDebounceMs = 30;

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

RoundDisplay display;
LvglDashboard dashboard;

bool sameModel(const UiModel& a, const UiModel& b) {
  return a.link == b.link && a.pairing == b.pairing &&
         a.locked == b.locked && a.charging == b.charging &&
         a.powerValid == b.powerValid &&
         a.chargePowerValid == b.chargePowerValid &&
         a.socValid == b.socValid && a.rangeValid == b.rangeValid &&
         a.temperatureValid == b.temperatureValid &&
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

}  // namespace

void DashboardApp::begin() {
  if (!display.begin()) {
    Serial.println("Display initialization failed.");
    return;
  }
  Serial.printf("Display ready, free heap: %u bytes\n", ESP.getFreeHeap());
  dashboard.begin();
  Serial.printf("UI built, free heap: %u bytes\n", ESP.getFreeHeap());
#if !CFG_DEMO_MODE
  m_sourceStarted = false;
  m_model.link = UiLinkState::Scanning;
#endif
  dashboard.render(m_model);
  Serial.printf("First frame, free heap: %u bytes\n", ESP.getFreeHeap());
  m_lastRenderedModel = m_model;
  m_hasRenderedModel = true;
  m_initialized = true;

  // BOOT 键平时是高电平，短按一次把画面顺时针旋转 90°。
  pinMode(kBootButtonPin, INPUT_PULLUP);
  beginPowerButton();
}

void DashboardApp::beginPowerButton() {
  Wire.begin(kPwrButtonSda, kPwrButtonScl);
  Wire.setClock(100000);

  const int16_t config = readExpanderReg(kPwrExpanderConfigReg);
  if (config < 0) {
    CFG_LOG("[BTN] TCA9554 not found; PWR long-press disabled");
    return;
  }
  // 配置寄存器里 1 = 输入。EXIO4 必须保持输入才能读回 PWR 状态。
  if ((config & kPwrButtonMask) == 0) {
    if (!writeExpanderReg(kPwrExpanderConfigReg,
                          static_cast<uint8_t>(config | kPwrButtonMask))) {
      CFG_LOG("[BTN] failed to configure TCA9554 EXIO4 as input");
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
      CFG_LOG("[BTN] PWR pressed (hold %ums to restart)",
              static_cast<unsigned>(kPwrLongPressMs));
    } else {
      // 松开后才允许下一次长按；这也让开机时按住的上电动作失效。
      m_pwrArmed = true;
      CFG_LOG("[BTN] PWR released");
    }
  }
  if (m_pwrPressed && m_pwrArmed &&
      now - m_pwrPressStartMs >= kPwrLongPressMs) {
    CFG_LOG("[BTN] PWR long press -> restart");
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
  // 电平稳定 kBootDebounceMs 后才算一次有效按下，避免抖动触发多次旋转。
  if (rawPressed != m_bootPressed &&
      now - m_bootLastChangeMs >= kBootDebounceMs) {
    m_bootPressed = rawPressed;
    if (m_bootPressed) {
      display.rotateClockwise();
    }
  }
}

void DashboardApp::setModel(const UiModel& model) { m_model = model; }

void DashboardApp::loop() {
  if (!m_initialized) {
    delay(1000);
    return;
  }
  pollBootButton();
  pollPowerButton();
#if !CFG_DEMO_MODE
  if (!m_sourceStarted && !dashboard.bootAnimationRunning()) {
    m_source.begin();
    m_sourceStarted = true;
  }
  if (m_sourceStarted) {
    m_source.loop();
    m_model = m_source.model();
  }
#endif
  const uint32_t now = millis();
  // The boot overlay hides everything, so redrawing the dashboard for its
  // duration would only compete with the ring animation and the BLE stack.
  if (!dashboard.bootAnimationRunning() && now - m_lastRenderMs >= 50) {
    const float speedInputKmh =
        m_model.speedValid ? m_model.speedKmh : 0.0f;
    if (!m_speedInitialized) {
      m_speedInitialized = true;
      m_speedInputKmh = speedInputKmh;
      m_displaySpeedKmh = speedInputKmh;
    } else if (speedInputKmh != m_speedInputKmh) {
      m_speedInputKmh = speedInputKmh;
      m_speedFromKmh = m_displaySpeedKmh;
      m_speedTransitionAt = now;
    }

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
  display.loop();
}
