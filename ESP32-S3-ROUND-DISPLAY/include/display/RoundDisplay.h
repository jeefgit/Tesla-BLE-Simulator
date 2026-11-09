#pragma once

#include <stdint.h>

#include <lvgl.h>

#include "display/IDisplay.h"

class Arduino_CO5300;

class RoundDisplay : public IDisplay {
 public:
  bool begin() override;
  void loop() override;

 private:
   static void flush(lv_display_t* display, const lv_area_t* area,
                    uint8_t* pixelMap);
  Arduino_CO5300* m_gfx = nullptr;
  lv_display_t* m_display = nullptr;
  void* m_drawBuffer = nullptr;
};
