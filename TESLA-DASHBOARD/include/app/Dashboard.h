#pragma once

#include "ble/BleLink.h"
#include "data/VehicleData.h"

// ---------------------------------------------------------------------------
// Dashboard：仪表渲染层
//
// 当前为"串口模拟表盘"，后续接 TFT/LVGL 时只改这个模块的实现，
// 数据来源依旧是 g_vehicle（见 data/VehicleData.h）。
// ---------------------------------------------------------------------------
class Dashboard {
 public:
  void begin();
  void render(const VehicleData& v, BleLink::State bleState, bool dataReady);

 private:
  uint32_t m_lastPrint = 0;
};
