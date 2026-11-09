#pragma once

#include "core/SourceStatus.h"
#include "core/VehicleState.h"

// ---------------------------------------------------------------------------
// IVehicleSource：后端数据源接口
//
// 换 ESP32 板子 / 屏幕时不需要动这里；换数据来源时只需新增一个实现：
//   - TeslaBleSource：真实特斯拉 BLE（加密握手 + GetVehicleData / 53B 旁路）
//   - MockSource：内建演示剧本
//
// 生命周期：
//   begin() 只调用一次；loop() 每次主循环调用；state()/status() 为只读快照。
// ---------------------------------------------------------------------------
class IVehicleSource {
 public:
  virtual ~IVehicleSource() = default;

  virtual void begin() = 0;
  virtual void loop() = 0;

  virtual const VehicleState& state() const = 0;
  virtual SourceStatus status() const = 0;
};
