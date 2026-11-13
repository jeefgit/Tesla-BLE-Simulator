#pragma once

#include <stddef.h>
#include <stdint.h>

#include <Wire.h>

// QMI8658 六轴 IMU 的最小 I2C 驱动，只实现自动休眠需要的功能：
//   - 读取三轴加速度（g）；
//   - 配置 Wake-on-Motion（WoM），让 INT2 在检测到运动时翻转；
//   - 读取 STATUS1 清除 WoM 事件并复位中断线。
//
// 地址 0x6B（SA0 拉高），与 CST9217 / TCA9554 / AXP2101 共用 Wire 总线。
// 不依赖第三方库，避免修改 platformio.ini。
class Qmi8658 {
 public:
  // wire 必须已经 begin()。内部会软复位芯片并配置为普通加速度计模式。
  bool begin(TwoWire& wire, uint8_t address = 0x6B);

  bool isPresent() const { return m_present; }

  // 读取加速度，单位 g（量程 ±8 g）。
  bool readAccel(float& x, float& y, float& z);

  // 配置 WoM：阈值单位 mg，blankingSamples 为启动时忽略的采样数。
  // 固定使用 INT2、初始电平 0（静止低、运动时翻转）。
  bool configureWakeOnMotion(uint8_t thresholdMg, uint8_t blankingSamples);

  // 读 STATUS1，清除 WoM 事件并把 INT2 复位到初始电平。
  bool clearWakeStatus();

 private:
  bool writeReg(uint8_t reg, uint8_t value);
  bool readRegs(uint8_t reg, uint8_t* data, size_t length);
  bool writeCommand(uint8_t command);
  bool waitCommandDone(bool done);

  TwoWire* m_wire = nullptr;
  uint8_t m_address = 0x6B;
  bool m_present = false;
};
