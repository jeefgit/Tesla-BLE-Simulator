#pragma once

#include "platform/BoardProfile.h"

// ---------------------------------------------------------------------------
// 板型选择：由 platformio.ini 的 build_flags 决定，默认 CYD。
//
// 新增板子：
//   1. 复制 platform/boards/cyd_2432s028.h 为 platform/boards/xxx.h
//   2. 在下面加一个 #elif defined(BOARD_XXX) 分支
//   3. platformio.ini 新增 env，build_flags 加 -DBOARD_XXX
// ---------------------------------------------------------------------------
#if defined(BOARD_ESP32S3_4848S040)
#include "platform/boards/esp32s3_4848s040.h"
#define BOARD_SELECTED BOARD_ESP32S3_4848S040
#else
// 默认板：ESP32-2432S028R（CYD 2.8" 320×240）
#include "platform/boards/cyd_2432s028.h"
#define BOARD_SELECTED BOARD_CYD_2432S028
#endif

namespace board {
inline constexpr BoardProfile kDefault = BOARD_SELECTED;
}  // namespace board
