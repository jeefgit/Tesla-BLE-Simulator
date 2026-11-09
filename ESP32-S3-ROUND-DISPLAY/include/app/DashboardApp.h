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
#if !CFG_DEMO_MODE
  TeslaUiSource m_source;
#endif
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
