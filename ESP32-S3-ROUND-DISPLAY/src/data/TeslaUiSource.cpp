#include "data/TeslaUiSource.h"

#include <string.h>

#include "config.h"

namespace {

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
  m_model.vin[0] = '\0';
  if (strlen(CFG_TESLA_VIN) < sizeof(m_model.vin)) {
    strncpy(m_model.vin, CFG_TESLA_VIN, sizeof(m_model.vin) - 1);
    m_model.vin[sizeof(m_model.vin) - 1] = '\0';
  }
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
  if (link != m_lastLink) {
    m_lastLink = link;
    m_stateSinceMs = now;
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
}

void TeslaUiSource::mapVehicle(const VehicleState& vehicle) {
  m_model.speedValid = vehicle.has(VF_SPEED);
  m_model.powerValid = vehicle.has(VF_MOTOR_POWER);
  m_model.chargePowerValid = vehicle.has(VF_CHARGE_POWER);
  m_model.socValid = vehicle.has(VF_SOC);
  m_model.rangeValid = vehicle.has(VF_RANGE);
  m_model.temperatureValid = vehicle.has(VF_TEMPS);

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
