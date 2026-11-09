#pragma once

#include "core/UiModel.h"

class IDashboard {
 public:
  virtual ~IDashboard() = default;
  virtual void begin() = 0;
  virtual void render(const UiModel& model) = 0;
};
