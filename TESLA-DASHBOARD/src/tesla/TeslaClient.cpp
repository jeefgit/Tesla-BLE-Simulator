#include "tesla/TeslaClient.h"

#include <mbedtls/sha1.h>
#include <string.h>

#include "config.h"
#include "tesla/PbCodec.h"

namespace {

// 数值合理性：范围检查同时天然排除了 NaN/Inf（NaN 与任何数比较都为 false）
inline bool inRange(float v, float lo, float hi) { return v >= lo && v <= hi; }

// ── CarServer.ChargeState ─────────────────────────────────────────
// 字段号见 tesla_protocol/protobuf/vehicle.proto
bool decodeChargeState(pb::Reader& r) {
  VehicleData& v = g_vehicle;
  uint32_t bits = 0;
  while (!r.atEnd()) {
    uint32_t f = 0, w = 0;
    if (!r.next(&f, &w)) break;

    if (f == 1 && w == pb::WIRE_LEN) {
      // charging_state：oneof 里的空消息，按字段号区分类型
      pb::Reader st;
      if (r.readMessage(&st)) {
        bool charging = false;
        while (!st.atEnd()) {
          uint32_t sf = 0, sw = 0;
          if (!st.next(&sf, &sw)) break;
          if (sw == pb::WIRE_LEN) {
            const uint8_t* p = nullptr;
            size_t n = 0;
            if (!st.readBytes(&p, &n)) break;
            if (sf == 4 || sf == 5) charging = true;  // Starting / Charging
          } else {
            st.skip(sw);
          }
        }
        v.charging = charging;
        bits |= VF_CHARGING;
      }
    } else if (f == 114 && w == pb::WIRE_VARINT) {
      uint64_t raw = 0;
      if (r.readVarint(&raw)) {
        v.soc = (int32_t)(uint32_t)raw;
        bits |= VF_SOC;
      }
    } else if (f == 111 && w == pb::WIRE_FIXED32) {
      float fv = 0;
      if (r.readFloat(&fv)) {
        v.rangeKm = fv;
        bits |= VF_RANGE;
      }
    } else if (f == 122 && w == pb::WIRE_VARINT) {
      uint64_t raw = 0;
      if (r.readVarint(&raw)) {
        v.chargePowerKw = (float)(int32_t)(uint32_t)raw;
        bits |= VF_CHARGE_POWER;
      }
    } else {
      r.skip(w);
    }
  }
  if (bits) v.mark(bits);
  return bits != 0;
}

// ── CarServer.ClimateState ────────────────────────────────────────
bool decodeClimateState(pb::Reader& r) {
  VehicleData& v = g_vehicle;
  uint32_t bits = 0;
  while (!r.atEnd()) {
    uint32_t f = 0, w = 0;
    if (!r.next(&f, &w)) break;

    if (f == 101 && w == pb::WIRE_FIXED32) {
      float fv = 0;
      if (r.readFloat(&fv)) {
        v.insideTempC = fv;
        bits |= VF_TEMPS;
      }
    } else if (f == 102 && w == pb::WIRE_FIXED32) {
      float fv = 0;
      if (r.readFloat(&fv)) {
        v.outsideTempC = fv;
        bits |= VF_TEMPS;
      }
    } else {
      r.skip(w);
    }
  }
  if (bits) v.mark(bits);
  return bits != 0;
}

// ── CarServer.DriveState ──────────────────────────────────────────
bool decodeDriveState(pb::Reader& r) {
  VehicleData& v = g_vehicle;
  uint32_t bits = 0;
  while (!r.atEnd()) {
    uint32_t f = 0, w = 0;
    if (!r.next(&f, &w)) break;

    if (f == 1 && w == pb::WIRE_LEN) {
      // shift_state：P=2 / R=3 / N=4 / D=5 / SNA=6
      pb::Reader st;
      if (r.readMessage(&st)) {
        char gear = '?';
        while (!st.atEnd()) {
          uint32_t sf = 0, sw = 0;
          if (!st.next(&sf, &sw)) break;
          if (sw == pb::WIRE_LEN) {
            const uint8_t* p = nullptr;
            size_t n = 0;
            if (!st.readBytes(&p, &n)) break;
            switch (sf) {
              case 2: gear = 'P'; break;
              case 3: gear = 'R'; break;
              case 4: gear = 'N'; break;
              case 5: gear = 'D'; break;
              default: gear = '?'; break;
            }
          } else {
            st.skip(sw);
          }
        }
        v.gear = gear;
        bits |= VF_GEAR;
      }
    } else if (f == 106 && w == pb::WIRE_FIXED32) {
      float fv = 0;
      if (r.readFloat(&fv)) {
        v.speedKmh = fv;
        bits |= VF_SPEED;
      }
    } else if (f == 102 && w == pb::WIRE_VARINT) {
      // 旧字段 speed 单位 mph，仅在没有 speed_float 时兜底
      if (!(bits & VF_SPEED)) {
        uint64_t raw = 0;
        if (r.readVarint(&raw)) {
          v.speedKmh = (float)(uint32_t)raw * 1.609344f;
          bits |= VF_SPEED;
        }
      } else {
        r.skip(w);
      }
    } else if (f == 103 && w == pb::WIRE_VARINT) {
      uint64_t raw = 0;
      if (r.readVarint(&raw)) {
        v.motorPowerKw = (float)(int32_t)(uint32_t)raw;
        bits |= VF_MOTOR_POWER;
      }
    } else {
      r.skip(w);
    }
  }
  if (bits) v.mark(bits);
  return bits != 0;
}

// ── CarServer.ClosuresState ───────────────────────────────────────
bool decodeClosuresState(pb::Reader& r) {
  VehicleData& v = g_vehicle;
  uint32_t bits = 0;
  while (!r.atEnd()) {
    uint32_t f = 0, w = 0;
    if (!r.next(&f, &w)) break;

    int doorIndex = -1;
    switch (f) {
      case 101: doorIndex = 0; break;  // driver front
      case 103: doorIndex = 1; break;  // passenger front
      case 102: doorIndex = 2; break;  // driver rear
      case 104: doorIndex = 3; break;  // passenger rear
      case 105: doorIndex = 4; break;  // trunk front
      case 106: doorIndex = 5; break;  // trunk rear
      default: break;
    }

    if (doorIndex >= 0) {
      if (w == pb::WIRE_VARINT) {
        uint64_t raw = 0;
        if (r.readVarint(&raw)) {
          v.doors[doorIndex] = raw ? 1 : 0;
          bits |= VF_DOORS;
        }
      } else {
        r.skip(w);
      }
    } else if (f == 113) {  // locked
      if (w == pb::WIRE_VARINT) {
        uint64_t raw = 0;
        if (r.readVarint(&raw)) {
          v.locked = raw != 0;
          bits |= VF_LOCK;
        }
      } else {
        r.skip(w);
      }
    } else {
      r.skip(w);
    }
  }
  if (bits) v.mark(bits);
  return bits != 0;
}

// ── CarServer.TirePressureState ───────────────────────────────────
bool decodeTirePressureState(pb::Reader& r) {
  VehicleData& v = g_vehicle;
  uint32_t bits = 0;
  while (!r.atEnd()) {
    uint32_t f = 0, w = 0;
    if (!r.next(&f, &w)) break;

    if (f >= 2 && f <= 5 && w == pb::WIRE_FIXED32) {
      float fv = 0;
      if (r.readFloat(&fv)) {
        v.tpms[f - 2] = fv;
        bits |= VF_TPMS;
      }
    } else {
      r.skip(w);
    }
  }
  if (bits) v.mark(bits);
  return bits != 0;
}

}  // namespace

void TeslaClient::begin(BleLink* link) {
  m_link = link;
  m_len = 0;
  m_packets = 0;
  m_lastPacketAt = 0;
  m_droppedBytes = 0;
  m_timeoutLogged = false;
  m_wasConnected = false;
  m_handshakeTries = 0;
  m_nextActionAt = 0;
  m_bypassIgnoredLogged = false;

  m_crypto.begin(String(CFG_TESLA_VIN));
  m_crypto.setResponseHandler([this](const uint8_t* p, size_t n, uint32_t fault) {
    onCryptoResponse(p, n, fault);
  });
  m_crypto.setSessionHandler([this](bool ok) { onCryptoSession(ok); });

#if CFG_TESLA_CRYPTO
  CFG_LOG("特斯拉加密协议栈就绪 VIN=%s（握手后改用加密 GetVehicleData 轮询）",
          CFG_TESLA_VIN);
#else
  CFG_LOG("特斯拉协议层就绪（加密关闭），只接收 %u 字节 SimPayload 帧",
          (unsigned)kFrame);
#endif
}

void TeslaClient::loop() {
  const bool connected = m_link && m_link->isConnected();
  if (!connected) {
    if (m_wasConnected) {
      m_wasConnected = false;
      onDisconnected();
    }
    return;
  }
  if (!m_wasConnected) {
    m_wasConnected = true;
    onConnected();
  }

#if CFG_TESLA_CRYPTO
  if (!m_crypto.hasSession()) {
    if ((int32_t)(millis() - m_nextActionAt) >= 0) sendHandshake();
  } else if ((int32_t)(millis() - m_nextActionAt) >= 0) {
    sendPoll();
  }
#else
  if (m_packets == 0 && !m_timeoutLogged) {
    CFG_LOG("已连接但尚未收到数据帧（检查 --vin、服务 UUID 与订阅）");
    m_timeoutLogged = true;
  }
#endif
}

void TeslaClient::onConnected() {
  m_len = 0;
  m_handshakeTries = 0;
  m_timeoutLogged = false;
  m_decodeLogged = false;
  m_bypassIgnoredLogged = false;
  m_crypto.reset();
#if CFG_TESLA_CRYPTO
  m_nextActionAt = millis();
  CFG_LOG("BLE 已连接，开始加密握手");
#endif
}

void TeslaClient::onDisconnected() {
  m_len = 0;
  m_handshakeTries = 0;
  m_crypto.reset();
  CFG_LOG("BLE 断开，加密会话已清除");
}

void TeslaClient::sendHandshake() {
  size_t n = 0;
  if (!m_crypto.buildHandshakeRequest(m_tx, sizeof(m_tx), &n)) {
    CFG_LOG("[CRYPTO] 握手请求构造失败");
    m_nextActionAt = millis() + CFG_TESLA_HANDSHAKE_RETRY_MS;
    return;
  }
  if (!m_link || !m_link->sendRaw(m_tx, n)) {
    CFG_LOG("[CRYPTO] 握手请求发送失败（链路未就绪）");
    m_nextActionAt = millis() + CFG_TESLA_HANDSHAKE_RETRY_MS;
    return;
  }

  ++m_handshakeTries;
  const uint32_t delay = m_handshakeTries >= CFG_TESLA_HANDSHAKE_MAX_RETRY
                             ? CFG_TESLA_HANDSHAKE_RETRY_LONG_MS
                             : CFG_TESLA_HANDSHAKE_RETRY_MS;
  m_nextActionAt = millis() + delay;
  CFG_LOG("[CRYPTO] 握手请求已发送 %uB（第 %u 次）", (unsigned)n,
          (unsigned)m_handshakeTries);
}

void TeslaClient::sendPoll() {
  size_t n = 0;
  if (!m_crypto.buildGetVehicleData(m_tx, sizeof(m_tx), &n)) {
    CFG_LOG("[CRYPTO] GetVehicleData 构造失败（会话已失效？）");
    m_nextActionAt = millis() + CFG_TESLA_HANDSHAKE_RETRY_MS;
    return;
  }
  if (!m_link || !m_link->sendRaw(m_tx, n)) {
    CFG_LOG("[CRYPTO] GetVehicleData 发送失败");
  }
  m_nextActionAt = millis() + CFG_TESLA_POLL_INTERVAL_MS;
}

void TeslaClient::onCryptoSession(bool ok) {
  if (ok) {
    m_handshakeTries = 0;
    m_nextActionAt = millis();  // 立即发第一条 GetVehicleData
  } else {
    m_nextActionAt = millis() + CFG_TESLA_HANDSHAKE_RETRY_MS;
  }
}

void TeslaClient::onCryptoResponse(const uint8_t* plaintext, size_t len,
                                   uint32_t fault) {
  if (fault != 0) {
    CFG_LOG("[CRYPTO] 协议层错误 fault=%u", (unsigned)fault);
    // 认证/域/epoch/过期：会话状态可能已不同步，丢弃后重新握手
    if (fault == 5 || fault == 6 || fault == 8 || fault == 15 || fault == 17) {
      m_crypto.reset();
      m_handshakeTries = 0;
      m_nextActionAt = millis() + CFG_TESLA_HANDSHAKE_RETRY_MS;
      CFG_LOG("[CRYPTO] 会话失效，准备重新握手");
    }
    return;
  }
  if (!plaintext || len == 0) return;

  if (decodeVehicleData(plaintext, len)) {
    ++m_packets;
    m_lastPacketAt = millis();
    m_timeoutLogged = false;
    if (!m_decodeLogged) {
      m_decodeLogged = true;
      CFG_LOG("[CRYPTO] 首帧 VehicleData 解密成功（%uB）", (unsigned)len);
    }
  }
}

bool TeslaClient::decodeVehicleData(const uint8_t* data, size_t len) {
  if (!data || len == 0) return false;

  bool ok = false;
  pb::Reader r(data, len);
  while (!r.atEnd()) {
    uint32_t f = 0, w = 0;
    if (!r.next(&f, &w)) break;

    if (f == 1 && w == pb::WIRE_LEN) {  // actionStatus
      pb::Reader st;
      if (r.readMessage(&st)) {
        uint32_t result = 0;
        while (!st.atEnd()) {
          uint32_t sf = 0, sw = 0;
          if (!st.next(&sf, &sw)) break;
          if (sf == 1 && sw == pb::WIRE_VARINT) {
            uint64_t raw = 0;
            if (st.readVarint(&raw)) result = (uint32_t)raw;
          } else {
            st.skip(sw);
          }
        }
        if (result != 0) {
          CFG_LOG("[CRYPTO] 应用层命令失败 result=%u", (unsigned)result);
        }
      }
    } else if (f == 2 && w == pb::WIRE_LEN) {  // vehicleData
      pb::Reader vd;
      if (!r.readMessage(&vd)) break;
      while (!vd.atEnd()) {
        uint32_t vf = 0, vw = 0;
        if (!vd.next(&vf, &vw)) break;
        pb::Reader sub;
        if (vf == 3 && vw == pb::WIRE_LEN) {
          if (vd.readMessage(&sub)) ok |= decodeChargeState(sub);
        } else if (vf == 4 && vw == pb::WIRE_LEN) {
          if (vd.readMessage(&sub)) ok |= decodeClimateState(sub);
        } else if (vf == 5 && vw == pb::WIRE_LEN) {
          if (vd.readMessage(&sub)) ok |= decodeDriveState(sub);
        } else if (vf == 9 && vw == pb::WIRE_LEN) {
          if (vd.readMessage(&sub)) ok |= decodeClosuresState(sub);
        } else if (vf == 19 && vw == pb::WIRE_LEN) {
          if (vd.readMessage(&sub)) ok |= decodeTirePressureState(sub);
        } else {
          vd.skip(vw);
        }
      }
    } else {
      r.skip(w);
    }
  }

  if (ok) g_vehicle.updatedAt = millis();
  return ok;
}

void TeslaClient::feed(const uint8_t* data, size_t len) {
  if (!data || len == 0) return;

  // 缓冲容量按单帧上限设计：一帧处理完立即消费，不会堆积多帧
  if (len > sizeof(m_buf) - m_len) {
    CFG_LOG("接收缓冲溢出，丢弃旧数据 %uB", (unsigned)m_len);
    m_len = 0;
    if (len > sizeof(m_buf)) len = sizeof(m_buf);
  }
  memcpy(m_buf + m_len, data, len);
  m_len += len;

  processBuffer();
}

void TeslaClient::processBuffer() {
  while (m_len >= 2) {
    const size_t declared =
        ((size_t)m_buf[0] << 8) | (size_t)m_buf[1];

#if CFG_TESLA_CRYPTO
    // 加密模式：必须先完成握手。握手前收到 53B 旁路帧一律丢弃，
    // 不解析、不写入 g_vehicle，避免出现"未握手就有数据"的假象。
    if (m_len >= kFrame) {
      TeslaPayload probe;
      memcpy(&probe, m_buf, kFrame);
      if (plausible(probe)) {
        consume(kFrame);
        if (!m_bypassIgnoredLogged) {
          m_bypassIgnoredLogged = true;
          CFG_LOG("[CRYPTO] 未握手成功，忽略 53B 旁路帧（必须先完成加密握手）");
        }
        continue;
      }
    }
#else
    // 1) 53B 旁路帧优先：旁路模式下每个 notify 就是完整一帧，
    //    其首 2 字节是 float 低位，不能当成加密帧长度前缀
    if (m_len >= kFrame) {
      TeslaPayload p;
      memcpy(&p, m_buf, kFrame);
      if (plausible(p)) {
        applyTesla(p);  // 写快照走 VehicleData 公共路径（与 MockSource 共用）
        consume(kFrame);
        ++m_packets;
        m_lastPacketAt = millis();
        m_timeoutLogged = false;
        continue;
      }
    }
#endif

    const bool cryptoHeader =
        declared > 0 && declared <= CFG_TESLA_MAX_FRAME;

    // 2) 长度前缀恰好吃满已收字节 -> 完整加密帧（2 字节长度 + RoutableMessage）
    if (cryptoHeader && m_len == declared + 2) {
      m_crypto.handleFrame(m_buf + 2, declared);
      consume(declared + 2);
      m_timeoutLogged = false;
      continue;
    }

    // 3) 加密帧分片（还未收全）-> 等后续 notify
    if (cryptoHeader && m_len < declared + 2) break;

    // 4) 前缀非法/过长且凑不够一个旁路帧：逐字节重同步
    if (m_len < kFrame) break;  // 可能是旁路半帧，等后续数据
    consume(1);
    ++m_droppedBytes;
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
#if CFG_TESLA_CRYPTO
  // 未握手成功时一律视为"无数据"，仪表走降级显示
  if (!m_crypto.hasSession()) return false;
#endif
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
