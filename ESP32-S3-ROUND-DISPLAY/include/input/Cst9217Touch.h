#pragma once

#include <stddef.h>
#include <stdint.h>

#include <Wire.h>

// CST9217（Hynitron）触摸控制器驱动，寄存器协议照抄 Waveshare BSP 使用的
// SensorLib TouchDrvCST92xx：写 0xD000 请求报告帧、读 15 字节、写 0xAB 应答。
// 只做轮询式单点读取（内部节流），供亮度滑动手势使用，不注册 LVGL 触摸输入。
class Cst9217Touch {
 public:
  // wire 必须已经 begin()；rstPin/intPin 可传 -1（轮询模式不依赖 INT）。
  bool begin(TwoWire& wire, uint8_t address, int8_t rstPin, int8_t intPin);

  // 轮询一次触摸状态。返回 true 表示有手指按下，坐标为面板原始方向；
  // 返回 false 时坐标无效。I2C 偶发失败时保持上一次状态。
  bool poll(int16_t& x, int16_t& y);

 private:
  bool writeBytes(const uint8_t* data, size_t length);
  bool writeThenRead(const uint8_t* writeData, size_t writeLength,
                     uint8_t* readData, size_t readLength);
  void hardwareReset();
  bool initController();

  TwoWire* m_wire = nullptr;
  uint8_t m_address = 0x5A;
  int8_t m_rstPin = -1;
  int8_t m_intPin = -1;
  bool m_ready = false;
  bool m_lastPressed = false;
  int16_t m_lastX = 0;
  int16_t m_lastY = 0;
  uint32_t m_lastPollMs = 0;
  uint8_t m_readErrors = 0;
};
