#include "data/TeslaBleSource.h"

#include <string.h>

#include "tesla/BackendConfig.h"

namespace {

// 日志脱敏：只返回最后 6 个字符；不足 6 位时返回原串。禁止打印完整 VIN。
const char* tail6(const char* vin) {
  if (vin == nullptr) return "";
  const size_t n = strlen(vin);
  return n > 6 ? vin + (n - 6) : vin;
}

}  // namespace

// BleLink::State -> 抽象 LinkState（UI 只认识后者）
static LinkState toLinkState(BleLink::State s) {
  switch (s) {
    case BleLink::State::Idle: return LinkState::Idle;
    case BleLink::State::Scanning: return LinkState::Scanning;
    case BleLink::State::Connecting: return LinkState::Connecting;
    case BleLink::State::Ready: return LinkState::Ready;
    case BleLink::State::Reconnecting: return LinkState::Reconnecting;
  }
  return LinkState::Idle;
}

void TeslaBleSource::setVin(const char* vin) { m_vin = vin ? vin : ""; }

void TeslaBleSource::begin() {
  // 上层未注入时回退出厂默认值，保证默认行为与编译期配置一致。
  if (m_vin.length() == 0) m_vin = CFG_TESLA_VIN;

  m_link.begin();

  // CFG_TESLA_NAME 非空会覆盖 VIN 推导的广播名：此时改 VIN 不影响扫描目标。
  if (strlen(CFG_TESLA_NAME) > 0) {
    CFG_LOG("[VIN] WARN CFG_TESLA_NAME overrides VIN-derived name; "
            "changing VIN will not change the scan target");
  }

  // 目标广播名：S + sha1(VIN) 前16位hex + C（与 sim_server.py 规则一致）
  String targetName =
      strlen(CFG_TESLA_NAME) > 0
          ? String(CFG_TESLA_NAME)
          : TeslaClient::advertisedNameFromVin(m_vin.c_str());
  if (targetName.length() == 0) {
    CFG_LOG("广播名推导失败（VIN 为空？），退回前缀/UUID 扫描");
  } else {
    m_link.setTargetName(targetName);
    // 只打印尾号，避免完整 VIN 进入串口日志。
    CFG_LOG("目标 VIN 尾号=%s 广播名=%s", tail6(m_vin.c_str()),
            targetName.c_str());
  }

  m_link.setRxHandler([this](const uint8_t* data, size_t len) {
    m_client.feed(data, len);
  });
  m_client.begin(&m_link, m_vin.c_str());
}

void TeslaBleSource::loop() {
  m_link.loop();
  m_client.loop();
}

SourceStatus TeslaBleSource::status() const {
  SourceStatus st;
  st.link = toLinkState(m_link.state());
  st.dataReady = m_client.ready();
  st.encrypted = m_client.encrypted();
  st.pairing = m_client.pairingHint();
  st.lastPacketMs = m_client.lastPacketMs();
  return st;
}
