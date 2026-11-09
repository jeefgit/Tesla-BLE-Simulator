#pragma once

#include "core/UiModel.h"

class DashboardApp {
 public:
  void begin();
  void loop();
  void setModel(const UiModel& model);

 private:
  UiModel m_model;
  bool m_initialized = false;
  uint32_t m_lastRenderMs = 0;
};
