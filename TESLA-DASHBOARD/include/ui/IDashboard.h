#pragma once

#include "core/UiModel.h"

// ---------------------------------------------------------------------------
// IDashboard：前端 UI 接口
//
// AppController 只依赖这个接口；换一块屏/一套布局时，新增一个实现即可
// （例如 DashboardLvgl320x240 / DashboardLvgl480x320）。
// ---------------------------------------------------------------------------
class IDashboard {
 public:
  virtual ~IDashboard() = default;

  // 创建 LVGL 对象树
  virtual void begin() = 0;

  // 用只读快照刷新界面（内部自行节流）
  virtual void render(const UiModel& model) = 0;
};
