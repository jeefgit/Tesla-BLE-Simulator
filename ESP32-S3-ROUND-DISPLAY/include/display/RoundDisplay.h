#pragma once

#include <stdint.h>

#include <lvgl.h>

#include "display/IDisplay.h"

class Arduino_CO5300;

class RoundDisplay : public IDisplay {
 public:
  bool begin() override;
  void loop() override;

  // 画面顺时针旋转 90°（0° → 90° → 180° → 270° → 0°）。
  // CO5300 的 MADCTL 只能做镜像、不能交换 XY，所以旋转在 flush 里用软件完成。
  void rotateClockwise();

 private:
  static void flush(lv_display_t* display, const lv_area_t* area,
                    uint8_t* pixelMap);
  void flushArea(lv_display_t* display, const lv_area_t* area,
                 uint8_t* pixelMap);
  void blitRotated(const uint16_t* src, uint16_t* dst, int32_t width,
                   int32_t height) const;

  Arduino_CO5300* m_gfx = nullptr;
  lv_display_t* m_display = nullptr;
  void* m_drawBuffer = nullptr;
  // 旋转时像素重排用的临时缓冲，大小与 m_drawBuffer 相同。
  void* m_rotateBuffer = nullptr;
  int32_t m_width = 0;
  int32_t m_height = 0;
  uint8_t m_rotation = 0;
};
