#include "app/Dashboard.h"

#include "config.h"

namespace {
const char* bleStateName(BleLink::State s) {
  switch (s) {
    case BleLink::State::Idle: return "空闲";
    case BleLink::State::Scanning: return "扫描中";
    case BleLink::State::Connecting: return "连接中";
    case BleLink::State::Ready: return "已连接";
    case BleLink::State::Reconnecting: return "重连中";
  }
  return "?";
}

// 车身开口标签（与 SimPayload doors[6] 顺序一致）
const char* const kDoorNames[6] = {"FL", "FR", "RL", "RR", "FRUNK", "TRUNK"};
}  // namespace

void Dashboard::begin() {
  m_lastPrint = 0;
  CFG_LOG("仪表层就绪（串口模拟表盘，接屏后替换 render）");
}

void Dashboard::render(const VehicleData& v, BleLink::State bleState,
                       bool dataReady) {
  const uint32_t now = millis();
  if (now - m_lastPrint < CFG_RENDER_INTERVAL_MS) return;
  m_lastPrint = now;

  // 数据可用 = 协议层仍在收帧 + 快照未超时；否则全部降级为 --
  const bool ok = dataReady && v.fresh(VF_SPEED, CFG_DATA_STALE_MS);

  char speed[12] = "--";
  char soc[8] = "--";
  char range[16] = "--";
  char gear[4] = "--";
  char power[16] = "--";
  char charge[20] = "--";
  char temps[32] = "--";
  char tpms[32] = "--";
  char lock[8] = "--";
  char doors[40] = "-";

  if (ok) {
    snprintf(speed, sizeof(speed), "%.0f", v.speedKmh);
    snprintf(soc, sizeof(soc), "%ld%%", (long)v.soc);
    snprintf(range, sizeof(range), "%.0f km", v.rangeKm);
    gear[0] = v.gear;
    gear[1] = '\0';
    snprintf(power, sizeof(power), "%+.1f kW", v.motorPowerKw);
    if (v.charging) {
      snprintf(charge, sizeof(charge), "%.1f kW", v.chargePowerKw);
    } else {
      snprintf(charge, sizeof(charge), "未充电");
    }
    snprintf(temps, sizeof(temps), "内%.1f 外%.1f", v.insideTempC,
             v.outsideTempC);
    snprintf(tpms, sizeof(tpms), "%.1f/%.1f/%.1f/%.1f", v.tpms[0], v.tpms[1],
             v.tpms[2], v.tpms[3]);
    snprintf(lock, sizeof(lock), "%s", v.locked ? "已锁" : "解锁");

    // 开口状态：有开的列出来，全关显示 "全关"
    doors[0] = '\0';
    size_t used = 0;
    for (size_t i = 0; i < 6; ++i) {
      if (!v.doors[i]) continue;
      const int n = snprintf(doors + used, sizeof(doors) - used, "%s%s",
                             used ? "/" : "", kDoorNames[i]);
      if (n <= 0 || (size_t)n >= sizeof(doors) - used) break;
      used += (size_t)n;
    }
    if (used == 0) snprintf(doors, sizeof(doors), "全关");
  }

  Serial.printf(
      "[仪表] BLE:%s | 数据:%s | 车速 %s km/h | SOC %s | 续航 %s | 档位 %s | 功率 %s\r\n",
      bleStateName(bleState), ok ? "就绪" : "----", speed, soc, range, gear,
      power);
  if (ok) {
    Serial.printf(
        "        充电:%s | 温度:%s | 胎压(FL/FR/RL/RR):%s | %s | 开口:%s\r\n",
        charge, temps, tpms, lock, doors);
  }
}
