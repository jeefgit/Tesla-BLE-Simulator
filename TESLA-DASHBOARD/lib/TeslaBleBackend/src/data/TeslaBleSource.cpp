#include "data/TeslaBleSource.h"

#include "tesla/BackendConfig.h"

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

void TeslaBleSource::begin() {
  m_link.begin();

  // 目标广播名：S + sha1(VIN) 前16位hex + C（与 sim_server.py 规则一致）
  String targetName =
      strlen(CFG_TESLA_NAME) > 0
          ? String(CFG_TESLA_NAME)
          : TeslaClient::advertisedNameFromVin(CFG_TESLA_VIN);
  if (targetName.length() == 0) {
    CFG_LOG("广播名推导失败（VIN 为空？），退回前缀/UUID 扫描");
  } else {
    m_link.setTargetName(targetName);
    CFG_LOG("目标 VIN=%s  广播名=%s", CFG_TESLA_VIN, targetName.c_str());
  }

  m_link.setRxHandler([this](const uint8_t* data, size_t len) {
    m_client.feed(data, len);
  });
  m_client.begin(&m_link);
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
