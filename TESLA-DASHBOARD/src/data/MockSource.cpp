#include "data/MockSource.h"

#include "config.h"
#include "data/VehicleData.h"

namespace {
// 与 sim_server.py generate_mock_frame() 完全一致的时间轴总长
constexpr uint32_t kLoopFrames = 1030;

// 按剧本帧号构造一帧 SimPayload（逐阶段对照 Python 源移植）
TeslaPayload makeFrame(uint32_t loopFrame) {
  TeslaPayload p{};

  // ---- 基线默认值（Python data 字典）----
  p.speed_kmh = 0.0f;
  p.motor_power_kw = 0.0f;
  p.battery_level = 62;
  p.battery_range_km = 248.0f;
  p.gear = 'P';
  memset(p.doors, 0, sizeof(p.doors));
  p.locked = 1;
  p.charging = 0;
  p.charge_power_kw = 0.0f;
  p.inside_temp = 35.0f;
  p.outside_temp = 16.0f;
  p.tpms[0] = 2.3f;  // fl
  p.tpms[1] = 2.3f;  // fr
  p.tpms[2] = 2.3f;  // rl
  p.tpms[3] = 2.3f;  // rr

  float r;

  if (loopFrame < 100) {
    // 阶段 0：解锁上车（车门开启, 胎压异常）
    p.locked = 0;
    p.doors[0] = 1;  // FL
    p.doors[2] = 1;  // RL
    p.tpms[0] = 2.4f;
    p.tpms[1] = 2.8f;
    p.tpms[2] = 3.3f;  // > 2.8 告警
    p.tpms[3] = 2.9f;  // > 2.8 告警
  } else if (loopFrame < 160) {
    // 阶段 1：准备起步（车门关闭, 电机微动）
    p.locked = 0;
    p.motor_power_kw = 1.5f;
    p.tpms[0] = 2.4f;
    p.tpms[1] = 2.8f;
    p.tpms[2] = 3.3f;
    p.tpms[3] = 2.9f;
  } else if (loopFrame < 240) {
    // 阶段 2：挂D挡起步加速中
    p.locked = 0;
    p.gear = 'D';
    if (loopFrame < 200) {
      p.speed_kmh = 0.0f;
      p.motor_power_kw = 0.0f;
    } else {
      r = (loopFrame - 200) / 40.0f;
      p.speed_kmh = r * 30.0f;
      p.motor_power_kw = 12.0f;
    }
    p.tpms[0] = 2.4f;
    p.tpms[1] = 2.8f;
    p.tpms[2] = 3.3f;
    p.tpms[3] = 2.9f;
  } else if (loopFrame < 360) {
    // 阶段 3：强力超车（120 km/h, 大功率输出）
    p.locked = 0;
    r = (loopFrame - 240) / 120.0f;
    p.gear = 'D';
    p.speed_kmh = 30.0f + r * 90.0f;
    p.motor_power_kw = 25.0f + r * 60.0f;
    p.tpms[0] = 2.4f;
    p.tpms[1] = 2.8f + r * 0.3f;
    p.tpms[2] = 3.3f;
    p.tpms[3] = 2.9f + r * 0.4f;
    p.inside_temp = 35.0f - r * 13.0f;
  } else if (loopFrame < 460) {
    // 阶段 4：强力能量回收制动（功率为负）
    p.locked = 0;
    p.gear = 'D';
    if (loopFrame < 410) {
      r = (loopFrame - 360) / 50.0f;
      p.speed_kmh = 120.0f - r * 30.0f;
      p.motor_power_kw = -12.5f;
    } else {
      r = (loopFrame - 410) / 50.0f;
      p.speed_kmh = 90.0f - r * 80.0f;
      p.motor_power_kw = -12.5f - r * 47.5f;
    }
    p.tpms[0] = 2.4f;
    p.tpms[1] = 3.1f;
    p.tpms[2] = 3.3f;
    p.tpms[3] = 3.3f;
  } else if (loopFrame < 580) {
    // 阶段 5：FSD 自动巡航中（gear='?' 表示巡航）
    p.locked = 0;
    p.gear = '?';
    if (loopFrame < 500) {
      p.speed_kmh = 80.0f;
      p.motor_power_kw = 8.5f;
    } else if (loopFrame < 540) {
      r = (loopFrame - 500) / 40.0f;
      p.speed_kmh = 80.0f + r * 5.0f;
      p.motor_power_kw = 12.0f;
    } else {
      r = (loopFrame - 540) / 40.0f;
      p.speed_kmh = 85.0f - r * 5.0f;
      p.motor_power_kw = 4.0f;
    }
    p.tpms[0] = 2.4f;
    p.tpms[1] = 3.1f;
    p.tpms[2] = 3.3f;
    p.tpms[3] = 3.3f;
  } else if (loopFrame < 700) {
    // 阶段 6：挂R挡倒车入库
    p.locked = 0;
    p.gear = 'R';
    if (loopFrame < 620) {
      p.speed_kmh = 0.0f;
      p.motor_power_kw = 0.0f;
    } else if (loopFrame < 660) {
      r = (loopFrame - 620) / 40.0f;
      p.speed_kmh = r * 6.0f;
      p.motor_power_kw = r * 5.0f;
    } else {
      r = (loopFrame - 660) / 40.0f;
      p.speed_kmh = 6.0f - r * 6.0f;
      p.motor_power_kw = 4.0f - r * 4.0f;
    }
    r = (loopFrame - 580) / 120.0f;
    p.tpms[0] = 2.4f;
    p.tpms[1] = 3.1f - r * 0.3f;
    p.tpms[2] = 3.3f;
    p.tpms[3] = 3.3f - r * 0.4f;
  } else if (loopFrame < 780) {
    // 阶段 7：抵达目的地, 驾驶员下车
    p.locked = 0;
    p.doors[0] = 1;  // FL
    p.tpms[0] = 2.4f;
    p.tpms[1] = 2.8f;
    p.tpms[2] = 3.3f;
    p.tpms[3] = 2.9f;
  } else {
    // 阶段 8：插枪充电中（7.2 kW 慢充）
    p.locked = 1;
    p.charging = 1;
    p.charge_power_kw = 7.2f;
    r = (loopFrame - 780) / 250.0f;
    const int32_t soc = 99 + (int32_t)(r * 2.0f);
    p.battery_level = soc > 100 ? 100 : soc;
    p.battery_range_km = 248.0f + r * 12.0f;
    p.tpms[0] = 2.4f;
    p.tpms[1] = 2.8f;
    p.tpms[2] = 3.3f;
    p.tpms[3] = 2.9f;
  }

  return p;
}

// 剧本阶段名（仅日志用，与 Python script_phase_desc 对应）
const char* phaseName(uint32_t loopFrame) {
  if (loopFrame < 100) return "阶段0: 解锁上车";
  if (loopFrame < 160) return "阶段1: 准备起步";
  if (loopFrame < 240) return "阶段2: 挂D起步";
  if (loopFrame < 360) return "阶段3: 强力超车";
  if (loopFrame < 460) return "阶段4: 能量回收";
  if (loopFrame < 580) return "阶段5: FSD巡航";
  if (loopFrame < 700) return "阶段6: 倒车入库";
  if (loopFrame < 780) return "阶段7: 开门下车";
  return "阶段8: 插枪充电";
}
}  // namespace

void MockSource::begin() {
  m_frame = 0;
  m_lastTick = millis();
  CFG_LOG("演示模式: 内建 9 阶段车况剧本, 节拍 %d ms (不启动 BLE)",
          CFG_MOCK_TICK_MS);
}

void MockSource::loop() {
  const uint32_t now = millis();
  if (now - m_lastTick < CFG_MOCK_TICK_MS) return;
  m_lastTick = now;

  const uint32_t loopFrame = m_frame % kLoopFrames;
  applyTesla(makeFrame(loopFrame));

  // 只在跨入新阶段时打一条日志（phaseName 每阶段返回不同常量，指针比较即可）
  static const char* lastPhase = nullptr;
  const char* phase = phaseName(loopFrame);
  if (phase != lastPhase) {
    CFG_LOG("[剧本] %s (帧 %u/%u)", phase, (unsigned)loopFrame,
            (unsigned)kLoopFrames);
    lastPhase = phase;
  }
  ++m_frame;
}
