#pragma once

#include <stdint.h>

// 双击屏幕截图：把当前 LVGL 画面渲染成 24bit BMP，按 rotation 旋转成屏幕
// 实际方向，再以 base64 从 USB 串口输出。前后带 ===SHOT-BEGIN/END=== 标记，
// PC 端用 tools/decode_screenshot.py 从串口日志里还原成 .bmp。
namespace screenshot {

// 抓取并输出一帧。
// rotation 取 RoundDisplay::rotation()（0~3，每档 90° 顺时针），保证图片方向
// 和用户看到的画面一致。
// 成功返回 true；LVGL 未开启 LV_USE_SNAPSHOT 或 PSRAM 不足时返回 false。
bool dumpToSerial(uint8_t rotation);

}  // namespace screenshot
