#pragma once

#include "platform/BoardProfile.h"

// ---------------------------------------------------------------------------
// IDisplay：屏幕平台接口
//
// AppController 只依赖这个接口，不关心是 LovyanGFX、无屏调试还是别的驱动。
// ---------------------------------------------------------------------------
class IDisplay {
 public:
  virtual ~IDisplay() = default;

  // 按板型配置初始化屏幕与 LVGL；成功返回 true
  virtual bool begin(const BoardProfile& board) = 0;

  // 每次主循环驱动一次 LVGL 事务
  virtual void loop() = 0;
};
