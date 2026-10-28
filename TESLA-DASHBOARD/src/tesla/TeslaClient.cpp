#include "tesla/TeslaClient.h"

#include <mbedtls/sha1.h>
#include <string.h>

#include "config.h"

namespace {
// 数值合理性：范围检查同时天然排除了 NaN/Inf（NaN 与任何数比较都为 false）
inline bool inRange(float v, float lo, float hi) { return v >= lo && v <= hi; }
}  // namespace

void TeslaClient::begin(BleLink* link) {
  m_link = link;
  m_len = 0;
  m_packets = 0;
  m_lastPacketAt = 0;
  m_droppedBytes = 0;
  m_timeoutLogged = false;
  CFG_LOG("特斯拉协议层就绪，等待 %u 字节 SimPayload 帧", (unsigned)kFrame);
}

void TeslaClient::loop() {
  // 连接就绪但长时间没收到帧：提示一次，方便排查广播名/UUID/订阅问题
  if (!m_link || !m_link->isConnected() || m_packets > 0) {
    m_timeoutLogged = false;
    return;
  }
  if (!m_timeoutLogged) {
    CFG_LOG("已连接但尚未收到数据帧（检查 --vin、服务 UUID 与订阅）");
    m_timeoutLogged = true;
  }
}

void TeslaClient::feed(const uint8_t* data, size_t len) {
  if (!data) return;
  for (size_t i = 0; i < len; ++i) {
    if (m_len >= kFrame) consume(kFrame);  // 防御：正常流程不会走到
    m_buf[m_len++] = data[i];

    if (m_len < kFrame) continue;

    TeslaPayload p;
    memcpy(&p, m_buf, kFrame);
    if (plausible(p)) {
      applyTesla(p);  // 写快照走 VehicleData 公共路径（与 MockSource 共用）
      m_len = 0;
      ++m_packets;
      m_lastPacketAt = millis();
      m_timeoutLogged = false;
    } else {
      // 帧失步（MTU 拆包 / 半帧后新帧到来）：丢弃首字节逐字节重同步
      consume(1);
      ++m_droppedBytes;
    }
  }
}

bool TeslaClient::plausible(const TeslaPayload& p) const {
  if (!inRange(p.speed_kmh, 0, 400)) return false;
  if (!inRange(p.motor_power_kw, -300, 1200)) return false;
  if (p.battery_level < 0 || p.battery_level > 100) return false;
  if (!inRange(p.battery_range_km, 0, 1200)) return false;
  if (p.gear != 'P' && p.gear != 'R' && p.gear != 'N' && p.gear != 'D' &&
      p.gear != '?') {
    return false;
  }
  if (p.locked > 1 || p.charging > 1) return false;
  for (size_t i = 0; i < 6; ++i)
    if (p.doors[i] > 1) return false;
  if (!inRange(p.charge_power_kw, 0, 500)) return false;
  if (!inRange(p.inside_temp, -50, 80)) return false;
  if (!inRange(p.outside_temp, -60, 60)) return false;
  for (size_t i = 0; i < 4; ++i)
    if (!inRange(p.tpms[i], 0, 6)) return false;
  return true;
}

void TeslaClient::consume(size_t n) {
  if (n >= m_len) {
    m_len = 0;
    return;
  }
  memmove(m_buf, m_buf + n, m_len - n);
  m_len -= n;
}

bool TeslaClient::ready() const {
  if (m_packets == 0) return false;
  return (millis() - m_lastPacketAt) < CFG_DATA_STALE_MS;
}

String TeslaClient::advertisedNameFromVin(const char* vin) {
  if (!vin || vin[0] == '\0') return String();

  // 与 sim_server.py 相同规则：S + sha1(VIN) 十六进制前16位 + C
  uint8_t digest[20];
  if (mbedtls_sha1((const unsigned char*)vin, strlen(vin), digest) != 0) {
    return String();
  }
  char hex[17];
  for (int i = 0; i < 8; ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]);
  hex[16] = '\0';

  String name("S");
  name += hex;
  name += 'C';
  return name;
}
