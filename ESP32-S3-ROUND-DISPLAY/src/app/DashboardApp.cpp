#include "app/DashboardApp.h"

#include <Arduino.h>

#include "config.h"
#include "display/RoundDisplay.h"
#include "ui/LvglDashboard.h"

namespace {

RoundDisplay display;
LvglDashboard dashboard;

}  // namespace

void DashboardApp::begin() {
  if (!display.begin()) {
    Serial.println("Display initialization failed.");
    return;
  }
  dashboard.begin();
#if !CFG_DEMO_MODE
  m_source.begin();
#endif
  dashboard.render(m_model);
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
  if (now - m_lastRenderMs >= 50) {
    dashboard.render(m_model);
    m_lastRenderMs = now;
  }
  display.loop();
}
