#include "input/Cst9217Touch.h"

#include <Arduino.h>

#include "config.h"

namespace {

// 报告帧布局（SensorLib MAX_FINGER_NUM * 5 + 5 = 15 字节）：
//   [0]      手指 0 头部：id = >>4，event = &0x0F
//   [1..3]   手指 0 坐标：x = ([1]<<4) | ([3]>>4)，y = ([2]<<4) | ([3]&0x0F)
//   [4]      手势字节（bit7 = 隔空覆盖手势）
//   [5]      手指数 & 0x7F
//   [6]      0xAB 校验标记
//   [7..]    手指 1 数据（本驱动只解析手指 0）
constexpr size_t kReportLength = 15;
constexpr uint8_t kAck = 0xAB;
constexpr uint8_t kEventContact = 0x06;
constexpr uint8_t kMaxFingerNum = 2;
constexpr uint32_t kPollIntervalMs = 30;
constexpr uint16_t kChipIdCst9217 = 0x9217;
constexpr uint16_t kChipIdCst9220 = 0x9220;

}  // namespace

bool Cst9217Touch::begin(TwoWire& wire, uint8_t address, int8_t rstPin,
                         int8_t intPin) {
  m_wire = &wire;
  m_address = address;
  m_rstPin = rstPin;
  m_intPin = intPin;

  if (m_rstPin >= 0) {
    pinMode(m_rstPin, OUTPUT);
    digitalWrite(m_rstPin, HIGH);
  }
  if (m_intPin >= 0) {
    // 轮询模式不读 INT，但保持引脚有确定的上拉状态。
    pinMode(m_intPin, INPUT_PULLUP);
  }

  hardwareReset();
  if (!initController()) {
    return false;
  }
  m_ready = true;
  return true;
}

void Cst9217Touch::hardwareReset() {
  if (m_rstPin < 0) {
    return;
  }
  digitalWrite(m_rstPin, LOW);
  delay(10);
  digitalWrite(m_rstPin, HIGH);
  // 触摸固件复位后需要时间起来，太快访问 I2C 会无应答。
  delay(100);
}

bool Cst9217Touch::writeBytes(const uint8_t* data, size_t length) {
  m_wire->beginTransmission(m_address);
  m_wire->write(data, length);
  return m_wire->endTransmission() == 0;
}

bool Cst9217Touch::writeThenRead(const uint8_t* writeData, size_t writeLength,
                                 uint8_t* readData, size_t readLength) {
  m_wire->beginTransmission(m_address);
  m_wire->write(writeData, writeLength);
  // CST9217 的寄存器读要求写和读之间发 STOP：Waveshare 自带的 SensorLib
  // 版本 sendStopFlag 默认为 true。用 repeated start 时芯片不应答。
  if (m_wire->endTransmission() != 0) {
    return false;
  }
  if (m_wire->requestFrom(m_address, static_cast<uint8_t>(readLength)) !=
      readLength) {
    return false;
  }
  for (size_t i = 0; i < readLength; ++i) {
    readData[i] = static_cast<uint8_t>(m_wire->read());
  }
  return true;
}

bool Cst9217Touch::initController() {
  // 复位后芯片处于 boot/命令模式，流程与 Waveshare 自带的 SensorLib
  // TouchDrvCST92xx 一致：0xD1 0x01 进命令模式，然后读校验码 / 分辨率 /
  // 芯片类型 / 固件版本。读完之后芯片即可直接输出触摸报告，不需要再切换模式。
  delay(30);

  uint8_t write[2];
  uint8_t read[8] = {0};

  write[0] = 0xD1;
  write[1] = 0x01;
  if (!writeBytes(write, 2)) {
    CFG_LOG("[TOUCH] failed to enter command mode");
    return false;
  }
  delay(10);

  write[0] = 0xD1;
  write[1] = 0xFC;
  if (!writeThenRead(write, 2, read, 4)) {
    CFG_LOG("[TOUCH] failed to read firmware info (0xD1FC)");
    return false;
  }
  CFG_LOG("[TOUCH] firmware info: %02X %02X %02X %02X", read[0], read[1],
          read[2], read[3]);
  // 固件信息校验码高 16 位固定为 CACA，读不到说明固件没起来。
  if (read[2] != 0xCA || read[3] != 0xCA) {
    CFG_LOG("[TOUCH] checkcode mismatch");
    return false;
  }

  write[0] = 0xD1;
  write[1] = 0xF8;
  if (!writeThenRead(write, 2, read, 4)) {
    CFG_LOG("[TOUCH] failed to read resolution (0xD1F8)");
    return false;
  }
  const uint16_t resX = static_cast<uint16_t>((read[1] << 8) | read[0]);
  const uint16_t resY = static_cast<uint16_t>((read[3] << 8) | read[2]);

  write[0] = 0xD2;
  write[1] = 0x04;
  if (!writeThenRead(write, 2, read, 4)) {
    CFG_LOG("[TOUCH] failed to read chip id (0xD204)");
    return false;
  }
  // 芯片类型是 16 位大端：高字节在后（SensorLib 写法 buffer[3]<<8 | buffer[2]）。
  const uint16_t chipId = static_cast<uint16_t>((read[3] << 8) | read[2]);
  if (chipId != kChipIdCst9217 && chipId != kChipIdCst9220) {
    CFG_LOG("[TOUCH] unexpected chip id 0x%04X", chipId);
    return false;
  }

  CFG_LOG("[TOUCH] %s ready (res %ux%u)",
          chipId == kChipIdCst9217 ? "CST9217" : "CST9220", resX, resY);
  return true;
}

bool Cst9217Touch::poll(int16_t& x, int16_t& y) {
  if (!m_ready) {
    return false;
  }
  const uint32_t now = millis();
  if (m_lastPollMs != 0 && now - m_lastPollMs < kPollIntervalMs) {
    x = m_lastX;
    y = m_lastY;
    return m_lastPressed;
  }
  m_lastPollMs = now;

  // 读报告帧，随后写 0xAB 应答让芯片释放 INT / 准备下一帧。
  uint8_t frame[kReportLength] = {0};
  const uint8_t readCmd[2] = {0xD0, 0x00};
  if (!writeThenRead(readCmd, 2, frame, kReportLength)) {
    // 总线偶发错误时保持上一次状态，避免手势被误判成松手。
    if (++m_readErrors == 5) {
      CFG_LOG("[TOUCH] I2C read failed repeatedly");
    }
    x = m_lastX;
    y = m_lastY;
    return m_lastPressed;
  }
  m_readErrors = 0;
  const uint8_t ackCmd[3] = {0xD0, 0x00, kAck};
  writeBytes(ackCmd, 3);

  bool pressed = false;
  // 与 Waveshare 版 SensorLib 一致：只有 [6] 的 0xAB 校验位有效才认为是
  // 有效报告帧，再按手指数和第一点的事件码（0x06 = 按下）判断。
  const bool coverGesture =
      (frame[4] & 0xF0) != 0 && (frame[4] >> 7) == 0x01;
  if (frame[6] == kAck && !coverGesture) {
    const uint8_t numPoints = frame[5] & 0x7F;
    if (numPoints > 0 && numPoints <= kMaxFingerNum &&
        (frame[0] & 0x0F) == kEventContact) {
      m_lastX = static_cast<int16_t>((frame[1] << 4) | (frame[3] >> 4));
      m_lastY = static_cast<int16_t>((frame[2] << 4) | (frame[3] & 0x0F));
      pressed = true;
    }
  }

  // 只在按下/松开时打印一帧原始数据，方便核对触摸方向和坐标。
  if (pressed != m_lastPressed) {
    CFG_LOG("[TOUCH] %s x=%d y=%d frame=%02X%02X%02X%02X%02X%02X%02X",
            pressed ? "press" : "release", m_lastX, m_lastY, frame[0],
            frame[1], frame[2], frame[3], frame[4], frame[5], frame[6]);
  }
  m_lastPressed = pressed;
  x = m_lastX;
  y = m_lastY;
  return pressed;
}
