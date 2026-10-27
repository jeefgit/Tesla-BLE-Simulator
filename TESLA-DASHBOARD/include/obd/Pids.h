#pragma once

#include <stdint.h>

// OBD 数据项的解析结果去向
enum class ObdMetric : uint8_t {
  Speed,       // 车速 km/h
  Rpm,         // 转速 rpm
  Throttle,    // 油门踏板 %
  Coolant,     // 冷却液温度 ℃
  Intake,      // 进气温度 ℃
  BattVoltage, // 模块电压 V
};

// 一条轮询指令
struct ObdPid {
  uint8_t mode;        // 诊断模式，0x01 = 当前数据
  uint8_t pid;         // PID 号
  ObdMetric metric;    // 解析后写入 VehicleData 的哪个字段
  const char* name;    // 调试用名称
};

// 轮询表（见 obd/Pids.cpp，后续可整体替换为特斯拉专用协议）
extern const ObdPid OBD_PID_TABLE[];
extern const uint8_t OBD_PID_COUNT;
