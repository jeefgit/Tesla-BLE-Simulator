#include "display/RoundDisplay.h"

#include <Arduino_GFX_Library.h>
#include <Preferences.h>
#include <esp_heap_caps.h>

#include <cstring>

#include "config.h"
#include "platform/boards/waveshare_esp32_s3_touch_amoled_175_b.h"

namespace {

Arduino_CO5300* s_gfx = nullptr;
bool s_brightnessReady = false;

// BOOT 键设置的旋转方向存在 NVS，掉电/重启后继续生效。
constexpr char kPrefsNamespace[] = "display";
constexpr char kPrefsRotationKey[] = "rot";

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
  m_width = board.width;
  m_height = board.height;

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

  // 旋转缓冲只做像素搬运，放 PSRAM 即可，避免占用 BLE 需要的内
  // 部 SRAM；如果板子没有 PSRAM 再退回内部 RAM。
  m_rotateBuffer =
      heap_caps_malloc(bufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (m_rotateBuffer == nullptr) {
    m_rotateBuffer =
        heap_caps_malloc(bufferBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  if (m_rotateBuffer == nullptr) {
    CFG_LOG("[UI] rotation buffer unavailable, rotation disabled");
  }

  // 恢复上次 BOOT 键设置的旋转方向，这样开机第一帧就是用户选的方向。
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, true)) {
    m_rotation = prefs.getUChar(kPrefsRotationKey, 0) & 0x03;
    prefs.end();
  }
  if (m_rotation != 0) {
    CFG_LOG("[UI] restored display rotation %u deg",
            static_cast<unsigned>(m_rotation) * 90);
  }

  lv_display_set_user_data(m_display, this);
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

void RoundDisplay::rotateClockwise() {
  if (m_rotateBuffer == nullptr) {
    return;
  }
  m_rotation = static_cast<uint8_t>((m_rotation + 1) & 0x03);
  CFG_LOG("[UI] display rotation -> %u deg",
          static_cast<unsigned>(m_rotation) * 90);
  // 立即写回 NVS，下次上电自动沿用这个方向。
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.putUChar(kPrefsRotationKey, m_rotation);
    prefs.end();
  }
  // 旋转只改变 flush 的写出方向，LVGL 并不知道画面已变化；整屏标脏
  // 让它用新的方向重新渲染一遍，否则旧方向的内容会留在屏幕上。
  if (m_display != nullptr) {
    lv_obj_invalidate(lv_screen_active());
  }
}

// 把宽 width、高 height 的 RGB565 像素块按 m_rotation 的方向重排。
// 输出块的行数/列数在 90°/270° 时会互换。
void RoundDisplay::blitRotated(const uint16_t* src, uint16_t* dst,
                               int32_t width, int32_t height) const {
  switch (m_rotation) {
    case 1:  // 顺时针 90°: (sx, sy) -> (height - 1 - sy, sx)
      for (int32_t sy = 0; sy < height; ++sy) {
        const uint16_t* row = src + sy * width;
        for (int32_t sx = 0; sx < width; ++sx) {
          dst[sx * height + (height - 1 - sy)] = row[sx];
        }
      }
      break;
    case 2:  // 180°: (sx, sy) -> (width - 1 - sx, height - 1 - sy)
      for (int32_t sy = 0; sy < height; ++sy) {
        for (int32_t sx = 0; sx < width; ++sx) {
          dst[(height - 1 - sy) * width + (width - 1 - sx)] =
              src[sy * width + sx];
        }
      }
      break;
    case 3:  // 顺时针 270°(= 逆时针 90°): (sx, sy) -> (sy, width - 1 - sx)
      for (int32_t sy = 0; sy < height; ++sy) {
        for (int32_t sx = 0; sx < width; ++sx) {
          dst[(width - 1 - sx) * height + sy] = src[sy * width + sx];
        }
      }
      break;
    default:
      std::memcpy(dst, src,
                  static_cast<size_t>(width) * height * sizeof(uint16_t));
      break;
  }
}

void RoundDisplay::flush(lv_display_t* display, const lv_area_t* area,
                         uint8_t* pixelMap) {
  auto* self = static_cast<RoundDisplay*>(lv_display_get_user_data(display));
  if (self == nullptr) {
    lv_display_flush_ready(display);
    return;
  }
  self->flushArea(display, area, pixelMap);
}

void RoundDisplay::flushArea(lv_display_t* display, const lv_area_t* area,
                             uint8_t* pixelMap) {
  if (s_gfx == nullptr) {
    lv_display_flush_ready(display);
    return;
  }

  int32_t x = area->x1;
  int32_t y = area->y1;
  int32_t width = area->x2 - area->x1 + 1;
  int32_t height = area->y2 - area->y1 + 1;
  uint16_t* pixels = reinterpret_cast<uint16_t*>(pixelMap);

  if (m_rotation != 0 && m_rotateBuffer != nullptr) {
    blitRotated(pixels, static_cast<uint16_t*>(m_rotateBuffer), width, height);
    switch (m_rotation) {
      case 1:
        x = m_height - area->y2 - 1;
        y = area->x1;
        break;
      case 2:
        x = m_width - area->x2 - 1;
        y = m_height - area->y2 - 1;
        break;
      case 3:
        x = area->y1;
        y = m_width - area->x2 - 1;
        break;
      default:
        break;
    }
    // 只有 90°/270° 会交换宽高；180° 的行列数不变，交换反而会把
    // 缓冲按错误步长解释，画面就会乱码。
    if ((m_rotation & 1) != 0) {
      const int32_t swapped = width;
      width = height;
      height = swapped;
    }
    pixels = static_cast<uint16_t*>(m_rotateBuffer);
  }

  s_gfx->draw16bitRGBBitmap(x, y, pixels, width, height);
  // Keep the panel muted until the first real frame is on screen, so the boot
  // sequence never exposes uninitialized panel memory.
  if (!s_brightnessReady) {
    s_brightnessReady = true;
    s_gfx->setBrightness(160);
  }
  lv_display_flush_ready(display);
}
