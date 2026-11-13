#include "input/Qmi8658.h"

#include <Arduino.h>

#include "config.h"

namespace {

constexpr uint8_t kRegWhoAmI = 0x00;
constexpr uint8_t kRegCtrl1 = 0x02;
constexpr uint8_t kRegCtrl2 = 0x03;
constexpr uint8_t kRegCtrl5 = 0x06;
constexpr uint8_t kRegCtrl7 = 0x08;
constexpr uint8_t kRegCtrl9 = 0x0A;
constexpr uint8_t kRegCal1L = 0x0B;
constexpr uint8_t kRegCal1H = 0x0C;
constexpr uint8_t kRegStatusInt = 0x2D;
constexpr uint8_t kRegStatus1 = 0x2F;
constexpr uint8_t kRegAxL = 0x35;
constexpr uint8_t kRegReset = 0x60;

constexpr uint8_t kChipId = 0x05;

// CTRL1: bit6 = 地址自增；bit4 = INT2 推挽输出使能。
constexpr uint8_t kCtrl1AutoIncrement = 0x40;
constexpr uint8_t kCtrl1Int2Enable = 0x10;

// CTRL2: bit[7:4] 量程，bit[3:0] ODR。
//   0x2 = ±8 g；0x8 = 31.25 Hz（普通模式）；0xC = 128 Hz 低功耗（WoM）。
constexpr uint8_t kCtrl2Normal = 0x28;
constexpr uint8_t kCtrl2Wom = 0x2C;

constexpr uint8_t kCtrl7AccelEnable = 0x01;
constexpr uint8_t kCtrl9WriteWomSetting = 0x08;
constexpr uint8_t kCtrl9Ack = 0x00;

// STATUSINT bit7 = 命令完成标志。
constexpr uint8_t kStatusIntCommandDone = 0x80;

// ±8 g 时灵敏度为 4096 LSB/g。
constexpr float kAccelLsbPerG = 4096.0f;

constexpr uint32_t kCommandTimeoutMs = 100;

}  // namespace

bool Qmi8658::begin(TwoWire& wire, uint8_t address) {
  m_wire = &wire;
  m_address = address;
  m_present = false;

  // 软复位，保证从 WoM 模式或任何未知状态回到干净配置。
  if (!writeReg(kRegReset, 0xB0)) {
    return false;
  }
  delay(15);

  uint8_t chipId = 0;
  if (!readRegs(kRegWhoAmI, &chipId, 1) || chipId != kChipId) {
    CFG_LOG("[IMU] QMI8658 not found at 0x%02X (id=0x%02X)", m_address, chipId);
    return false;
  }

  // 普通加速度计模式：地址自增、±8 g、31.25 Hz、只开加速度计。
  if (!writeReg(kRegCtrl1, kCtrl1AutoIncrement) ||
      !writeReg(kRegCtrl2, kCtrl2Normal) || !writeReg(kRegCtrl5, 0) ||
      !writeReg(kRegCtrl7, kCtrl7AccelEnable)) {
    return false;
  }
  // 清掉可能残留的 WoM 事件。
  clearWakeStatus();

  m_present = true;
  CFG_LOG("[IMU] QMI8658 ready (id=0x%02X)", chipId);
  return true;
}

bool Qmi8658::readAccel(float& x, float& y, float& z) {
  if (!m_present) {
    return false;
  }
  uint8_t data[6] = {0};
  if (!readRegs(kRegAxL, data, sizeof(data))) {
    return false;
  }
  const int16_t rawX = static_cast<int16_t>((data[1] << 8) | data[0]);
  const int16_t rawY = static_cast<int16_t>((data[3] << 8) | data[2]);
  const int16_t rawZ = static_cast<int16_t>((data[5] << 8) | data[4]);
  x = static_cast<float>(rawX) / kAccelLsbPerG;
  y = static_cast<float>(rawY) / kAccelLsbPerG;
  z = static_cast<float>(rawZ) / kAccelLsbPerG;
  return true;
}

bool Qmi8658::configureWakeOnMotion(uint8_t thresholdMg,
                                    uint8_t blankingSamples) {
  if (!m_present) {
    return false;
  }
  // 阈值 0 表示关闭 WoM，因此至少写 1 mg。
  if (thresholdMg == 0) {
    thresholdMg = 1;
  }

  // 1. 关闭传感器。
  if (!writeReg(kRegCtrl7, 0x00)) {
    return false;
  }
  // 2. 低功耗 ODR + ±8 g。
  if (!writeReg(kRegCtrl2, kCtrl2Wom)) {
    return false;
  }
  // 3. WoM 阈值（1 mg/LSB）。
  if (!writeReg(kRegCal1L, thresholdMg)) {
    return false;
  }
  // 4. CAL1_H: bit[7:6] = 01 -> INT2、初始电平 0；bit[5:0] = blanking。
  const uint8_t cal1h =
      static_cast<uint8_t>(0x40 | (blankingSamples & 0x3F));
  if (!writeReg(kRegCal1H, cal1h)) {
    return false;
  }
  // 5. 打开 INT2 推挽输出。
  uint8_t ctrl1 = 0;
  if (!readRegs(kRegCtrl1, &ctrl1, 1)) {
    return false;
  }
  ctrl1 |= kCtrl1AutoIncrement | kCtrl1Int2Enable;
  if (!writeReg(kRegCtrl1, ctrl1)) {
    return false;
  }
  // 6. 执行 WoM 配置命令。
  if (!writeCommand(kCtrl9WriteWomSetting)) {
    return false;
  }
  // 7. STATUS1 清事件，再回 ACK 让芯片完成命令。
  uint8_t status = 0;
  readRegs(kRegStatus1, &status, 1);
  if (!writeCommand(kCtrl9Ack)) {
    return false;
  }
  // 8. WoM 模式只开加速度计。
  if (!writeReg(kRegCtrl7, kCtrl7AccelEnable)) {
    return false;
  }

  // 配置期间若已产生事件，清掉，避免休眠瞬间被立刻唤醒。
  clearWakeStatus();
  CFG_LOG("[IMU] WoM configured (threshold=%umg, blanking=%u)",
          static_cast<unsigned>(thresholdMg),
          static_cast<unsigned>(blankingSamples));
  return true;
}

bool Qmi8658::clearWakeStatus() {
  if (m_wire == nullptr) {
    return false;
  }
  uint8_t status = 0;
  // 读 STATUS1 本身就会清除 WoM 位并复位 INT 线。
  return readRegs(kRegStatus1, &status, 1);
}

bool Qmi8658::writeReg(uint8_t reg, uint8_t value) {
  if (m_wire == nullptr) {
    return false;
  }
  m_wire->beginTransmission(m_address);
  m_wire->write(reg);
  m_wire->write(value);
  return m_wire->endTransmission() == 0;
}

bool Qmi8658::readRegs(uint8_t reg, uint8_t* data, size_t length) {
  if (m_wire == nullptr || data == nullptr) {
    return false;
  }
  m_wire->beginTransmission(m_address);
  m_wire->write(reg);
  if (m_wire->endTransmission(false) != 0) {
    return false;
  }
  if (m_wire->requestFrom(m_address, static_cast<uint8_t>(length)) != length) {
    return false;
  }
  for (size_t i = 0; i < length; ++i) {
    data[i] = static_cast<uint8_t>(m_wire->read());
  }
  return true;
}

bool Qmi8658::writeCommand(uint8_t command) {
  if (!writeReg(kRegCtrl9, command)) {
    return false;
  }
  // WOM 命令：等待 STATUSINT.bit7 置位表示完成。
  if (command != kCtrl9Ack) {
    return waitCommandDone(true);
  }
  // ACK 命令：等待 STATUSINT.bit7 清零表示处理完毕。
  return waitCommandDone(false);
}

bool Qmi8658::waitCommandDone(bool expectDone) {
  const uint32_t start = millis();
  while (millis() - start < kCommandTimeoutMs) {
    delay(1);
    uint8_t statusInt = 0;
    if (!readRegs(kRegStatusInt, &statusInt, 1)) {
      return false;
    }
    const bool done = (statusInt & kStatusIntCommandDone) != 0;
    if (done == expectDone) {
      return true;
    }
  }
  CFG_LOG("[IMU] CTRL9 command timeout");
  return false;
}
