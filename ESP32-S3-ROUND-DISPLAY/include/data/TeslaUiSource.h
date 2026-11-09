#pragma once

#include <TeslaBleBackend.h>

#include "core/UiModel.h"
#include "data/IUiModelSource.h"

// 把 TeslaBleBackend 的 VehicleState / SourceStatus 映射成圆屏的 UiModel。
class TeslaUiSource : public IUiModelSource {
 public:
  void begin() override;
  void loop() override;
  const UiModel& model() const override { return m_model; }

 private:
  void mapStatus(const SourceStatus& status, uint32_t now);
  void mapVehicle(const VehicleState& vehicle);

  TeslaBleBackend m_backend;
  UiModel m_model;
  UiLinkState m_lastLink = UiLinkState::Idle;
  uint32_t m_stateSinceMs = 0;
};
