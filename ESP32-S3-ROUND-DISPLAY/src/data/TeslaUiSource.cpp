#include "data/TeslaUiSource.h"

#include <Arduino.h>
#include <string.h>

#include "config.h"
#include "config/VinStore.h"
#include "log/EventLog.h"

namespace {

// 日志脱敏：只返回最后 6 个字符；不足 6 位时返回原串。禁止打印完整 VIN。
const char* tail6(const char* vin) {
  if (vin == nullptr) return "";
  const size_t n = strlen(vin);
  return n > 6 ? vin + (n - 6) : vin;
}

const char* linkName(UiLinkState link) {
  switch (link) {
    case UiLinkState::Idle:
      return "Idle";
    case UiLinkState::Scanning:
      return "Scanning";
    case UiLinkState::Connecting:
      return "Connecting";
    case UiLinkState::Syncing:
      return "Syncing";
    case UiLinkState::Ready:
      return "Ready";
    case UiLinkState::Reconnecting:
      return "Reconnecting";
    case UiLinkState::Error:
      return "Error";
  }
  return "?";
}

// 进入 Error / 配对失败时补一行上下文：字段全部取自已有快照，不新增 BLE 请求。
// 用于事后区分是信号问题、配对被拒，还是堆不足/长时间运行后异常。
void logLinkContext(const UiModel& model) {
  EVT_LOGI("CTX", "link=%s heap=%u uptime=%lus speed=%.0f soc=%ld pairing=%d",
           linkName(model.link), static_cast<unsigned>(ESP.getFreeHeap()),
           static_cast<unsigned long>(millis() / 1000), model.speedKmh,
           static_cast<long>(model.soc), static_cast<int>(model.pairing));
}

UiLinkState toUiLink(LinkState link, bool encrypted, bool pairing) {
  switch (link) {
    case LinkState::Idle:
      return UiLinkState::Idle;
    case LinkState::Scanning:
      return UiLinkState::Scanning;
    case LinkState::Connecting:
      return UiLinkState::Connecting;
    case LinkState::Ready:
      // 链路通了但握手还没完成时先显示 SYNCING；配钥提示期间保持
      // Ready，配对页才不会被断连页顶掉。
      return (encrypted || pairing) ? UiLinkState::Ready : UiLinkState::Syncing;
    case LinkState::Reconnecting:
      return UiLinkState::Reconnecting;
    case LinkState::Error:
      return UiLinkState::Error;
  }
  return UiLinkState::Idle;
}

UiPairingPhase toUiPairing(PairingHint::Phase phase) {
  switch (phase) {
    case PairingHint::TapCard:
      return UiPairingPhase::TapCard;
    case PairingHint::WaitConfirm:
      return UiPairingPhase::WaitConfirm;
    case PairingHint::Retry:
      return UiPairingPhase::Retry;
    case PairingHint::Success:
      return UiPairingPhase::Success;
    case PairingHint::Failed:
      return UiPairingPhase::Failed;
    case PairingHint::Idle:
      break;
  }
  return UiPairingPhase::Idle;
}

}  // namespace

void TeslaUiSource::begin() {
  m_stateSinceMs = millis();

  // VIN 优先级由 vinstore 决定（NVS 覆盖 > 出厂默认），后端也注入同一个值。
  const char* vin = vinstore::current();
  m_model.vin[0] = '\0';
  if (strlen(vin) < sizeof(m_model.vin)) {
    strncpy(m_model.vin, vin, sizeof(m_model.vin) - 1);
    m_model.vin[sizeof(m_model.vin) - 1] = '\0';
  }
  if (vin[0] != '\0') {
    EVT_LOGI("VIN", "apply tail=%s", tail6(vin));
  }

  m_backend.setVin(vin);
  m_backend.begin();
}

void TeslaUiSource::loop() {
  m_backend.loop();

  const uint32_t now = millis();
  const SourceStatus status = m_backend.status();
  mapStatus(status, now);
  mapVehicle(m_backend.state());
}

void TeslaUiSource::mapStatus(const SourceStatus& status, uint32_t now) {
  const bool pairing = status.pairing.phase != PairingHint::Idle;
  const UiLinkState link = toUiLink(status.link, status.encrypted, pairing);
  bool needContext = false;
  if (link != m_lastLink) {
    // 只在状态切换时记录，上游再快也不会刷屏（evtlog 本身还有 500ms 限频）。
    EVT_LOGI("BLE", "link %s -> %s", linkName(m_lastLink), linkName(link));
    m_lastLink = link;
    m_stateSinceMs = now;
    if (link == UiLinkState::Error) {
      EVT_LOGE("BLE", "link Error encrypted=%d pairing=%d attempt=%u/%u",
               status.encrypted ? 1 : 0,
               static_cast<unsigned>(status.pairing.phase),
               static_cast<unsigned>(status.pairing.attempt),
               static_cast<unsigned>(status.pairing.maxAttempts));
      needContext = true;
    }
  }

  m_model.link = link;
  m_model.pairing = toUiPairing(status.pairing.phase);
  m_model.pairingRemainMs = status.pairing.remainMs;
  if (status.pairing.attempt > 0) {
    m_model.pairingAttempt = status.pairing.attempt;
  }
  if (status.pairing.maxAttempts > 0) {
    m_model.pairingMaxAttempts = status.pairing.maxAttempts;
  }
  m_model.elapsedSeconds = (now - m_stateSinceMs) / 1000;

  if (status.pairing.attempt > 0 &&
      status.pairing.attempt != m_lastPairingAttempt) {
    EVT_LOGW("PAIR", "attempt=%u/%u phase=%u",
             static_cast<unsigned>(status.pairing.attempt),
             static_cast<unsigned>(status.pairing.maxAttempts),
             static_cast<unsigned>(status.pairing.phase));
    m_lastPairingAttempt = status.pairing.attempt;
  }
  if (m_model.pairing == UiPairingPhase::Failed &&
      m_lastPairingPhase != UiPairingPhase::Failed) {
    EVT_LOGE("PAIR", "failed info=%lu attempt=%u/%u",
             static_cast<unsigned long>(status.pairing.info),
             static_cast<unsigned>(status.pairing.attempt),
             static_cast<unsigned>(status.pairing.maxAttempts));
    needContext = true;
  }
  m_lastPairingPhase = m_model.pairing;

  if (needContext) {
    logLinkContext(m_model);
  }
}

void TeslaUiSource::mapVehicle(const VehicleState& vehicle) {
  m_model.speedValid = vehicle.has(VF_SPEED);
  m_model.powerValid = vehicle.has(VF_MOTOR_POWER);
  m_model.chargePowerValid = vehicle.has(VF_CHARGE_POWER);
  m_model.socValid = vehicle.has(VF_SOC);
  m_model.rangeValid = vehicle.has(VF_RANGE);
  m_model.temperatureValid = vehicle.has(VF_TEMPS);
  m_model.doorsValid = vehicle.has(VF_DOORS);
  for (uint8_t i = 0; i < 6; ++i) {
    m_model.doors[i] = m_model.doorsValid && vehicle.doors[i] != 0;
  }

  m_model.speedKmh = vehicle.speedKmh;
  m_model.motorPowerKw = vehicle.motorPowerKw;
  m_model.chargePowerKw = vehicle.chargePowerKw;
  m_model.soc = vehicle.soc;
  m_model.rangeKm = static_cast<int32_t>(vehicle.rangeKm + 0.5f);
  m_model.insideTempC = vehicle.insideTempC;
  m_model.outsideTempC = vehicle.outsideTempC;
  m_model.gear = vehicle.has(VF_GEAR) ? vehicle.gear : '?';
  m_model.charging = vehicle.has(VF_CHARGING) && vehicle.charging;
  // 未知锁状态时按已上锁显示，避免误报 UNLOCK。
  m_model.locked = !vehicle.has(VF_LOCK) || vehicle.locked;
}
