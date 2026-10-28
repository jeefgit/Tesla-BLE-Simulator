#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// Display：屏幕驱动层（2.8 寸 ST7789 320×240 + LVGL 9）
//
// 职责：
//   begin()  初始化 LovyanGFX 面板 / 背光 / LVGL（含显示缓冲与刷新回调）
//   loop()   每次主循环驱动一次 LVGL 事务（内部按需重绘）
// 对上层（Dashboard）只暴露这两个函数 + lv_display 句柄，
// UI 代码完全通过 LVGL API 绘制，不直接碰驱动。
// ---------------------------------------------------------------------------
class Display {
 public:
  void begin();
  void loop();

  bool ok() const { return m_ok; }

 private:
  bool m_ok = false;
};
