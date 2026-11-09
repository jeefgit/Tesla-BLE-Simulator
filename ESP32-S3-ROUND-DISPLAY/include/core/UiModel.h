#pragma once

#include <stdint.h>

enum class UiLinkState : uint8_t {
  Idle,
  Scanning,
  Connecting,
  Syncing,
  Ready,
  Reconnecting,
  Error,
};

enum class UiPairingPhase : uint8_t {
  Idle,
  TapCard,
  WaitConfirm,
  Retry,
  Success,
  Failed,
};

struct UiModel {
  UiLinkState link = UiLinkState::Ready;
  UiPairingPhase pairing = UiPairingPhase::Idle;
  bool locked = true;
  bool charging = false;
  bool speedValid = true;
  bool powerValid = true;
  bool chargePowerValid = true;
  bool socValid = true;
  bool rangeValid = true;
  bool temperatureValid = true;
  float speedKmh = 128.0f;
  float motorPowerKw = 44.0f;
  float chargePowerKw = 11.2f;
  int32_t soc = 78;
  int32_t rangeKm = 312;
  float insideTempC = 22.0f;
  float outsideTempC = 18.0f;
  char gear = 'D';
  uint32_t pairingRemainMs = 30000;
  uint8_t pairingAttempt = 1;
  uint8_t pairingMaxAttempts = 3;
  uint32_t elapsedSeconds = 12;
  char vin[18] = "LRWYGCFJ8RC571213";
};
