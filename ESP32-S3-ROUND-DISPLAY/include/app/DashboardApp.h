#pragma once

#include "config.h"
#include "core/UiModel.h"

#if !CFG_DEMO_MODE
#include "data/TeslaUiSource.h"
#endif

class DashboardApp {
 public:
  void begin();
  void loop();
  void setModel(const UiModel& model);

 private:
  void pollBootButton();
  void beginPowerButton();
  void pollPowerButton();

#if !CFG_DEMO_MODE
  TeslaUiSource m_source;
  bool m_sourceStarted = false;
#endif
  bool m_bootRawPressed = false;
  bool m_bootPressed = false;
  uint32_t m_bootLastChangeMs = 0;
  bool m_pwrAvailable = false;
  bool m_pwrArmed = false;
  bool m_pwrRawPressed = false;
  bool m_pwrPressed = false;
  uint32_t m_pwrLastChangeMs = 0;
  uint32_t m_pwrPressStartMs = 0;
  UiModel m_model;
  bool m_initialized = false;
  uint32_t m_lastRenderMs = 0;
  UiModel m_lastRenderedModel;
  bool m_hasRenderedModel = false;
  bool m_speedInitialized = false;
  float m_speedInputKmh = 0;
  float m_displaySpeedKmh = 0;
  float m_speedFromKmh = 0;
  uint32_t m_speedTransitionAt = 0;
};
