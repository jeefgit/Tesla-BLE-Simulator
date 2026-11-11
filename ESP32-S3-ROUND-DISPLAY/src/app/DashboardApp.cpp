#include "app/DashboardApp.h"

#include <Arduino.h>
#include <string.h>

#include "config.h"
#include "display/RoundDisplay.h"
#include "ui/LvglDashboard.h"

namespace {

constexpr uint32_t kSpeedTransitionMs = 30;

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
  m_source.begin();
#endif
  dashboard.render(m_model);
  Serial.printf("First frame, free heap: %u bytes\n", ESP.getFreeHeap());
  m_lastRenderedModel = m_model;
  m_hasRenderedModel = true;
  m_initialized = true;
}

void DashboardApp::setModel(const UiModel& model) { m_model = model; }

void DashboardApp::loop() {
  if (!m_initialized) {
    delay(1000);
    return;
  }
#if !CFG_DEMO_MODE
  m_source.loop();
  m_model = m_source.model();
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
