#pragma once

#include <Arduino.h>

#include "platform/BoardProfile.h"
#include "platform/IDisplay.h"

// ---------------------------------------------------------------------------
// Display：LovyanGFX 屏幕驱动层（默认 2.8 寸 ST7789/ILI9341 320×240 + LVGL 9）
//
// 职责：
//   begin(board)  按 BoardProfile 初始化面板 / 背光 / LVGL（含显示缓冲与刷新回调）
//   loop()        每次主循环驱动一次 LVGL 事务（内部按需重绘）
// 对上层（AppController / UI）只暴露这两个函数，
// UI 代码完全通过 LVGL API 绘制，不直接碰驱动。
//
// 换板/换屏：新增 platform/boards/xxx.h 填 BoardProfile，不改本文件。
// ---------------------------------------------------------------------------
class Display : public IDisplay {
 public:
  bool begin(const BoardProfile& board) override;
  void loop() override;

  bool ok() const { return m_ok; }

 private:
  bool m_ok = false;
};
