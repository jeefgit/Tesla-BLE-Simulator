#pragma once

#include <Arduino.h>
#include <stdint.h>

// 只前向声明协议帧：领域模型不依赖 tesla/ 的具体协议实现。
struct TeslaPayload;

// 字段有效位，配合 VehicleState::valid 使用
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
// VehicleState：车辆实时数据快照（领域模型，不依赖 UI / 传输层）
//
// 数据生产者：data/ 下的 IVehicleSource 实现
// 数据消费者：app/AppController -> ui/IDashboard
// ---------------------------------------------------------------------------
struct VehicleState {
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

// 兼容旧代码：VehicleData 是 VehicleState 的别名。
using VehicleData = VehicleState;

// 全局快照。后续如需多数据源/可测试性，可改为由 IVehicleSource 持有，
// UI 只通过 UiModel 读取。
extern VehicleState g_vehicle;

// 把一帧 SimPayload 写入全局快照（TeslaClient 与 MockSource 共用同一路径，
// 保证 UI 侧无论真实 BLE 还是演示数据行为一致）
void applyTesla(const TeslaPayload& p);
