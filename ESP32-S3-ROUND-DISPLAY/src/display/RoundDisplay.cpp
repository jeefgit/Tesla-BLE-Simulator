#include "display/RoundDisplay.h"

#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>

#include "platform/boards/waveshare_esp32_s3_touch_amoled_175_b.h"

namespace {

Arduino_CO5300* s_gfx = nullptr;
bool s_brightnessReady = false;

uint32_t getLvglTick() { return millis(); }

// CO5300 的 CASET/PASET 窗口按 2 像素对齐更稳（Waveshare BSP 也这样做）：
// 起点向下取偶、终点向上取奇，避免奇数列/行触发面板窗口异常。
void roundInvalidArea(lv_event_t* event) {
  auto* area = static_cast<lv_area_t*>(lv_event_get_param(event));
  if (area == nullptr) {
    return;
  }
  area->x1 &= ~1;
  area->y1 &= ~1;
  area->x2 |= 1;
  area->y2 |= 1;
}

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

  // CO5300's init sequence ends with DISPON at brightness 0xD0, but its GRAM
  // still holds power-on garbage. That garbage is visible as colored patches
  // until LVGL's first full frame arrives, so mute the panel and clear GRAM to
  // black synchronously. Brightness is restored after the first LVGL frame.
  m_gfx->setBrightness(0);
  m_gfx->fillScreen(0x0000);

  lv_init();
  lv_tick_set_cb(getLvglTick);
  m_display = lv_display_create(board.width, board.height);
  if (m_display == nullptr) {
    return false;
  }

  // 50 行与 Waveshare BSP 默认一致；优先放内部 SRAM（LVGL 软件渲染写
  // PSRAM 会明显变慢），空间不足再退回 PSRAM。
  constexpr uint32_t kBufferLines = 50;
  const uint32_t bufferBytes =
      static_cast<uint32_t>(board.width) * kBufferLines * sizeof(uint16_t);
  m_drawBuffer =
      heap_caps_malloc(bufferBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (m_drawBuffer == nullptr) {
    m_drawBuffer =
        heap_caps_malloc(bufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  if (m_drawBuffer == nullptr) {
    return false;
  }

  lv_display_set_color_format(m_display, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(m_display, RoundDisplay::flush);
  lv_display_set_buffers(m_display, m_drawBuffer, nullptr, bufferBytes,
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_add_event_cb(m_display, roundInvalidArea,
                          LV_EVENT_INVALIDATE_AREA, nullptr);
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
  // Keep the panel muted until the first real frame is on screen, so the boot
  // sequence never exposes uninitialized panel memory.
  if (!s_brightnessReady) {
    s_brightnessReady = true;
    s_gfx->setBrightness(160);
  }
  lv_display_flush_ready(display);
}
