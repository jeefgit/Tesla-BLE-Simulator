#pragma once

#include <stdint.h>

// ---------------------------------------------------------------------------
// TeslaPayload：模拟器 sim_server.py 的 53 字节遥测帧
//
// Python 侧 struct.pack("<ffifc6BBfff4f", ...)，小端序、紧凑无对齐空洞：
//
//   偏移  字段              类型
//   0-3   speed_kmh         float    实时时速
//   4-7   motor_power_kw    float    电机功率（负=动能回收）
//   8-11  battery_level     int32    SOC (%)
//   12-15 battery_range_km  float    续航 (km)
//   16    gear              char     P/R/N/D/?（? = 巡航）
//   17-22 doors[6]          u8×6     fl,fr,rl,rr,frunk,trunk
//   23    locked            u8       中控锁
//   24    charging          u8       充电状态
//   25-28 charge_power_kw   float    充电功率
//   29-32 inside_temp       float    车内温度
//   33-36 outside_temp      float    车外温度
//   37-52 tpms[4]           float×4  四轮胎压 (bar)
//
// 接收端按"帧长 == 53"识别裸 SimPayload（见 docs/HANDOVER.md 旁路说明）。
// ---------------------------------------------------------------------------
struct __attribute__((packed)) TeslaPayload {
  float speed_kmh;
  float motor_power_kw;
  int32_t battery_level;
  float battery_range_km;
  char gear;
  uint8_t doors[6];
  uint8_t locked;
  uint8_t charging;
  float charge_power_kw;
  float inside_temp;
  float outside_temp;
  float tpms[4];
};

static_assert(sizeof(TeslaPayload) == 53, "SimPayload must be exactly 53 bytes");
