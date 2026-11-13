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

  // 当前旋转档位（0~3）。触摸手势需要它把屏幕坐标换算回画面坐标。
  uint8_t rotation() const { return m_rotation; }

  // 亮度对应 CO5300 的 0x51 寄存器，范围 0~255。setBrightness 只写寄存器
  // （拖动过程中每次滑动都调用），saveBrightness 才写 NVS，避免磨损 flash。
  void setBrightness(uint8_t brightness);
  void saveBrightness();
  uint8_t brightness() const { return m_brightness; }

  // 把触摸芯片报告的面板物理坐标换算成 LVGL 画面坐标（blitRotated 的逆变换）。
  // 无论画面被旋转到哪个角度，"屏幕上的上下"始终对应画面坐标的 y 轴。
  void mapTouchToScreen(int16_t& x, int16_t& y) const;

 private:
  static constexpr uint8_t kDefaultBrightness = 160;
  // 亮度限制在 10%~80%，避免调到最亮刺眼或最暗看不清。
  static constexpr uint8_t kMinBrightness = 26;   // 255 * 10%
  static constexpr uint8_t kMaxBrightness = 204;  // 255 * 80%

  static uint8_t clampBrightness(uint8_t brightness) {
    if (brightness < kMinBrightness) {
      return kMinBrightness;
    }
    if (brightness > kMaxBrightness) {
      return kMaxBrightness;
    }
    return brightness;
  }

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
  uint8_t m_brightness = kDefaultBrightness;
};
