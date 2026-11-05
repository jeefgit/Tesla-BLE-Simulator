#pragma once

#include <Arduino.h>

#include "ble/BleLink.h"
#include "data/IVehicleSource.h"
#include "tesla/TeslaClient.h"

// ---------------------------------------------------------------------------
// TeslaBleSource：特斯拉 BLE 数据源（默认后端）
//
// 组合 BleLink + TeslaClient，把 BLE 链路状态映射成抽象的 LinkState /
// SourceStatus，向上只暴露 IVehicleSource 接口。
//
// 加密路径：握手成功后周期 GetVehicleData；握手失败时按 CFG_TESLA_PAIR_ON_DENIED
// 走配钥提示。旁路路径（CFG_TESLA_CRYPTO=0）直接收 53B SimPayload。
// ---------------------------------------------------------------------------
class TeslaBleSource : public IVehicleSource {
 public:
  void begin() override;
  void loop() override;

  const VehicleState& state() const override { return g_vehicle; }
  SourceStatus status() const override;

 private:
  BleLink m_link;
  TeslaClient m_client;
};
