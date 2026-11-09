#pragma once

#include "core/UiModel.h"

class IVehicleSource {
 public:
  virtual ~IVehicleSource() = default;
  virtual void begin() = 0;
  virtual void loop() = 0;
  virtual const UiModel& model() const = 0;
};
