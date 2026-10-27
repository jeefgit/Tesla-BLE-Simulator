#include "obd/Pids.h"

// ---------------------------------------------------------------------------
// 轮询表：按顺序循环发送
//
// 说明：
// - 这里是标准 OBD-II (Mode 01) PID，适配通用 ELM327 盒子。
// - 特斯拉原厂 OBD 口对标准 PID 支持很有限（车速/电量等很多取不到），
//   拿到盒子后若数据不对，把这里换成盒子支持的 PID（或 Tesla 专用协议）即可，
//   上层 ObdClient / VehicleData 结构不用动。
// ---------------------------------------------------------------------------
const ObdPid OBD_PID_TABLE[] = {
    {0x01, 0x0D, ObdMetric::Speed, "车速"},
    {0x01, 0x0C, ObdMetric::Rpm, "转速"},
    {0x01, 0x11, ObdMetric::Throttle, "油门踏板"},
    {0x01, 0x05, ObdMetric::Coolant, "冷却液温度"},
    {0x01, 0x0F, ObdMetric::Intake, "进气温度"},
    {0x01, 0x42, ObdMetric::BattVoltage, "模块电压"},
};

const uint8_t OBD_PID_COUNT =
    sizeof(OBD_PID_TABLE) / sizeof(OBD_PID_TABLE[0]);
