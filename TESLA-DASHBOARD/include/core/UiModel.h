#pragma once

#include "core/SourceStatus.h"
#include "core/VehicleState.h"

// ---------------------------------------------------------------------------
// UiModel：一次 UI 渲染所需的全部输入（只读快照）
//
// 这是前后端之间唯一的契约：
//   - 后端（data/）产出 VehicleState + SourceStatus
//   - AppController 组装成 UiModel
//   - 前端（ui/）只读 UiModel，不反向调用任何后端对象
// ---------------------------------------------------------------------------
struct UiModel {
  VehicleState vehicle;
  SourceStatus status;
  bool demoMode = false;  // 演示模式（UI 可用于隐藏 BLE 相关提示）
};
