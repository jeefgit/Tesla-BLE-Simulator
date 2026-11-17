#include "display/ScreenShot.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <lvgl.h>

#include <cstring>

#include "config.h"

// 串口截图期间静音其他日志；见 include/config.h 里的声明。
namespace cfglog {
namespace {
volatile bool s_muted = false;
}  // namespace

void setMuted(bool muted) { s_muted = muted; }
bool muted() { return s_muted; }
}  // namespace cfglog

#if CFG_SCREENSHOT_ENABLE

namespace {

constexpr char kBeginMarker[] = "===SHOT-BEGIN===";
constexpr char kEndMarker[] = "===SHOT-END===";
constexpr char kBase64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// BMP 头固定 54 字节（14 字节文件头 + 40 字节 DIB 头）。
constexpr uint32_t kHeaderBytes = 54;

void putU16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

void putU32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
  p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
  p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

// 把 LVGL 逻辑坐标下的一帧 RGB565 画进 BMP。
// 旋转映射与 RoundDisplay::blitRotated 一致：
//   0°: (sx, sy) -> (sx, sy)
//   90°: (sx, sy) -> (h-1-sy, sx)
//   180°: (sx, sy) -> (w-1-sx, h-1-sy)
//   270°: (sx, sy) -> (sy, w-1-sx)
// BMP 的像素行是自下而上存储的（height 为正），所以写入时再翻转一次。
void fillBmpPixels(const uint16_t* src, int32_t srcWidth, int32_t srcHeight,
                   int32_t srcStridePx, uint8_t rotation, int32_t dstWidth,
                   int32_t dstHeight, uint32_t rowBytes, uint8_t* bmp) {
  uint8_t* pixels = bmp + kHeaderBytes;
  for (int32_t row = 0; row < dstHeight; ++row) {
    // BMP 第 0 行是图片最下面一行。
    const int32_t dy = dstHeight - 1 - row;
    uint8_t* out = pixels + static_cast<size_t>(row) * rowBytes;
    for (int32_t dx = 0; dx < dstWidth; ++dx, out += 3) {
      int32_t sx = dx;
      int32_t sy = dy;
      switch (rotation) {
        case 1:
          sx = dy;
          sy = srcHeight - 1 - dx;
          break;
        case 2:
          sx = srcWidth - 1 - dx;
          sy = srcHeight - 1 - dy;
          break;
        case 3:
          sx = srcWidth - 1 - dy;
          sy = dx;
          break;
        default:
          break;
      }
      if (sx < 0 || sx >= srcWidth || sy < 0 || sy >= srcHeight) {
        continue;
      }
      const uint16_t v =
          src[static_cast<size_t>(sy) * srcStridePx + static_cast<size_t>(sx)];
      const uint8_t r5 = static_cast<uint8_t>((v >> 11) & 0x1F);
      const uint8_t g6 = static_cast<uint8_t>((v >> 5) & 0x3F);
      const uint8_t b5 = static_cast<uint8_t>(v & 0x1F);
      // 5/6 bit 展开到 8 bit，保留最亮值。
      out[0] = static_cast<uint8_t>((b5 * 255 + 15) / 31);
      out[1] = static_cast<uint8_t>((g6 * 255 + 31) / 63);
      out[2] = static_cast<uint8_t>((r5 * 255 + 15) / 31);
    }
  }
}

// 逐段 base64 输出，避免为 65 万字符再申请一块内存。
// 每行带 "SHOT:" 前缀：万一有其他日志插进来，解码脚本只认带前缀的行。
void writeBase64(const uint8_t* data, size_t length) {
  constexpr size_t kPrefixLen = 5;   // "SHOT:"
  constexpr size_t kChunkChars = 240;
  char line[kPrefixLen + kChunkChars + 2];
  size_t used = kPrefixLen;
  std::memcpy(line, "SHOT:", kPrefixLen);
  for (size_t i = 0; i < length; i += 3) {
    const size_t remain = length - i;
    uint32_t v = static_cast<uint32_t>(data[i]) << 16;
    if (remain > 1) {
      v |= static_cast<uint32_t>(data[i + 1]) << 8;
    }
    if (remain > 2) {
      v |= data[i + 2];
    }
    line[used++] = kBase64[(v >> 18) & 0x3F];
    line[used++] = kBase64[(v >> 12) & 0x3F];
    line[used++] = (remain > 1) ? kBase64[(v >> 6) & 0x3F] : '=';
    line[used++] = (remain > 2) ? kBase64[v & 0x3F] : '=';
    if (used >= kPrefixLen + kChunkChars) {
      line[used++] = '\n';
      Serial.write(reinterpret_cast<const uint8_t*>(line), used);
      used = kPrefixLen;
    }
  }
  if (used > kPrefixLen) {
    line[used++] = '\n';
    Serial.write(reinterpret_cast<const uint8_t*>(line), used);
  }
}

}  // namespace

bool screenshot::dumpToSerial(uint8_t rotation) {
  // 先让 LVGL 把待刷新的内容画完，保证抓到的是当前状态。
  lv_refr_now(nullptr);

  lv_draw_buf_t* snap =
      lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
  if (snap == nullptr) {
    CFG_LOG("[SHOT] snapshot failed (LV_USE_SNAPSHOT? out of PSRAM?)");
    return false;
  }

  const int32_t srcWidth = static_cast<int32_t>(snap->header.w);
  const int32_t srcHeight = static_cast<int32_t>(snap->header.h);
  const int32_t srcStridePx = static_cast<int32_t>(snap->header.stride) / 2;
  const uint16_t* src = reinterpret_cast<const uint16_t*>(snap->data);

  rotation &= 0x03;
  const bool swapped = (rotation & 1) != 0;
  const int32_t dstWidth = swapped ? srcHeight : srcWidth;
  const int32_t dstHeight = swapped ? srcWidth : srcHeight;

  // BMP 每行 4 字节对齐。
  const uint32_t rowBytes =
      (static_cast<uint32_t>(dstWidth) * 3u + 3u) & ~3u;
  const uint32_t pixelBytes = rowBytes * static_cast<uint32_t>(dstHeight);
  const uint32_t fileBytes = kHeaderBytes + pixelBytes;

  auto* bmp = static_cast<uint8_t*>(
      heap_caps_malloc(fileBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (bmp == nullptr) {
    bmp = static_cast<uint8_t*>(heap_caps_malloc(fileBytes, MALLOC_CAP_8BIT));
  }
  if (bmp == nullptr) {
    CFG_LOG("[SHOT] bmp buffer alloc failed (%lu bytes)",
            static_cast<unsigned long>(fileBytes));
    lv_draw_buf_destroy(snap);
    return false;
  }
  std::memset(bmp, 0, fileBytes);

  // BITMAPFILEHEADER
  bmp[0] = 'B';
  bmp[1] = 'M';
  putU32(bmp + 2, fileBytes);
  putU16(bmp + 6, 0);  // reserved
  putU16(bmp + 8, 0);
  putU32(bmp + 10, kHeaderBytes);
  // BITMAPINFOHEADER
  putU32(bmp + 14, 40);
  putU32(bmp + 18, static_cast<uint32_t>(dstWidth));
  putU32(bmp + 22, static_cast<uint32_t>(dstHeight));
  putU16(bmp + 26, 1);   // planes
  putU16(bmp + 28, 24);  // bpp
  putU32(bmp + 30, 0);   // BI_RGB
  putU32(bmp + 34, pixelBytes);
  putU32(bmp + 38, 2835);  // 72 dpi
  putU32(bmp + 42, 2835);
  putU32(bmp + 46, 0);
  putU32(bmp + 50, 0);

  fillBmpPixels(src, srcWidth, srcHeight, srcStridePx, rotation, dstWidth,
                dstHeight, rowBytes, bmp);

  CFG_LOG("[SHOT] %dx%d bytes=%lu rotation=%u", dstWidth, dstHeight,
          static_cast<unsigned long>(fileBytes),
          static_cast<unsigned>(rotation) * 90u);
  // 输出期间静音其他日志，避免 BLE 回调的日志插进标记行或 base64 行。
  cfglog::setMuted(true);
  // 标记单独占一行，方便脚本正则匹配。
  Serial.printf("\r\n%s\r\n", kBeginMarker);
  Serial.printf("w=%d h=%d bytes=%lu\r\n", dstWidth, dstHeight,
                static_cast<unsigned long>(fileBytes));
  writeBase64(bmp, fileBytes);
  Serial.printf("\r\n%s\r\n", kEndMarker);
  Serial.flush();
  cfglog::setMuted(false);

  heap_caps_free(bmp);
  lv_draw_buf_destroy(snap);
  CFG_LOG("[SHOT] done, free heap: %u bytes", ESP.getFreeHeap());
  return true;
}

#else  // CFG_SCREENSHOT_ENABLE

bool screenshot::dumpToSerial(uint8_t rotation) {
  (void)rotation;
  return false;
}

#endif  // CFG_SCREENSHOT_ENABLE
