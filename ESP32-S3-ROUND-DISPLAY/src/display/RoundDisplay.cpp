#include "display/RoundDisplay.h"

#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>

#include "platform/boards/waveshare_esp32_s3_touch_amoled_175_b.h"

namespace {

Arduino_CO5300* s_gfx = nullptr;

uint32_t getLvglTick() { return millis(); }

}  // namespace

bool RoundDisplay::begin() {
  const BoardProfile& board = kWaveshareEsp32S3TouchAmoled175B;
  auto* bus = new Arduino_ESP32QSPI(
      board.pinLcdCs, board.pinLcdPclk, board.pinLcdSio0, board.pinLcdSio1,
      board.pinLcdSio2, board.pinLcdSio3);
  m_gfx = new Arduino_CO5300(bus, board.pinLcdReset, 0, board.width,
                             board.height, board.panelOffsetX,
                             board.panelOffsetY, 0, 0);
  s_gfx = m_gfx;

  if (!m_gfx->begin()) {
    return false;
  }

  m_gfx->setBrightness(160);
  lv_init();
  lv_tick_set_cb(getLvglTick);
  m_display = lv_display_create(board.width, board.height);
  if (m_display == nullptr) {
    return false;
  }

  constexpr uint32_t kBufferLines = 48;
  const uint32_t bufferBytes =
      static_cast<uint32_t>(board.width) * kBufferLines * sizeof(uint16_t);
  m_drawBuffer =
      heap_caps_malloc(bufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (m_drawBuffer == nullptr) {
    m_drawBuffer =
        heap_caps_malloc(bufferBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  if (m_drawBuffer == nullptr) {
    return false;
  }

  lv_display_set_color_format(m_display, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(m_display, RoundDisplay::flush);
  lv_display_set_buffers(m_display, m_drawBuffer, nullptr, bufferBytes,
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_default(m_display);
  return true;
}

void RoundDisplay::loop() { lv_timer_handler(); }

void RoundDisplay::flush(lv_display_t* display, const lv_area_t* area,
                         uint8_t* pixelMap) {
  if (s_gfx == nullptr) {
    lv_display_flush_ready(display);
    return;
  }

  const int32_t width = area->x2 - area->x1 + 1;
  const int32_t height = area->y2 - area->y1 + 1;
  s_gfx->draw16bitRGBBitmap(area->x1, area->y1,
                            reinterpret_cast<uint16_t*>(pixelMap), width,
                            height);
  lv_display_flush_ready(display);
}
