#pragma once

#include <Arduino.h>
#include <stdint.h>

#include "tesla/TeslaPayload.h"

// 字段有效位，配合 VehicleData::valid 使用
enum VehicleField : uint32_t {
  VF_SPEED = 1u << 0,
  VF_RPM = 1u << 1,
  VF_THROTTLE = 1u << 2,
  VF_COOLANT = 1u << 3,
  VF_INTAKE = 1u << 4,
  VF_BATT_V = 1u << 5,

  // 特斯拉 SimPayload 字段
  VF_MOTOR_POWER = 1u << 6,   // motor_power_kw
  VF_SOC = 1u << 7,           // battery_level (%)
  VF_RANGE = 1u << 8,         // battery_range_km
  VF_GEAR = 1u << 9,          // gear (P/R/N/D/?)
  VF_DOORS = 1u << 10,        // doors[6] 开口状态
  VF_LOCK = 1u << 11,         // locked
  VF_CHARGING = 1u << 12,     // charging
  VF_CHARGE_POWER = 1u << 13, // charge_power_kw
  VF_TEMPS = 1u << 14,        // inside/outside_temp
  VF_TPMS = 1u << 15,         // tpms[4]
};

// ---------------------------------------------------------------------------
// 车辆实时数据快照 —— 全局唯一数据源
// 数据生产者：tesla/TeslaClient 或 obd/ObdClient   数据消费者：app/Dashboard
// ---------------------------------------------------------------------------
struct VehicleData {
  uint32_t valid = 0;      // bit 为 1 表示该字段已有数据
  uint32_t updatedAt = 0;  // 最近一次更新的 millis()

  // OBD 数据源
  float speedKmh = 0;
  uint16_t rpm = 0;
  float throttlePct = 0;
  float coolantC = 0;
  float intakeC = 0;
  float battVoltage = 0;

  // 特斯拉 SimPayload 数据源（speedKmh 复用）
  float motorPowerKw = 0;
  int32_t soc = 0;
  float rangeKm = 0;
  char gear = '?';
  uint8_t doors[6] = {0, 0, 0, 0, 0, 0};  // fl, fr, rl, rr, frunk, trunk
  bool locked = false;
  bool charging = false;
  float chargePowerKw = 0;
  float insideTempC = 0;
  float outsideTempC = 0;
  float tpms[4] = {0, 0, 0, 0};  // fl, fr, rl, rr

  bool has(uint32_t field) const { return (valid & field) == field; }
  void mark(uint32_t field) { valid |= field; }

  // 该字段是否在 ageMs 内更新过（用来判断数据是否"过期"）
  bool fresh(uint32_t field, uint32_t ageMs) const {
    return has(field) && (millis() - updatedAt) < ageMs;
  }
};

extern VehicleData g_vehicle;

// 把一帧 SimPayload 写入全局快照（TeslaClient 与 MockSource 共用同一路径，
// 保证 UI 侧无论真实 BLE 还是演示数据行为一致）
void applyTesla(const TeslaPayload& p);
