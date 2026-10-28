#pragma once

#include <Arduino.h>

#include "tesla/TeslaPayload.h"

// ---------------------------------------------------------------------------
// MockSource：演示模式数据源（屏幕开发期临时用，CFG_DEMO_MODE=1）
//
// 移植 sim_server.py 的 generate_mock_frame() 9 阶段车况剧本：
//   阶段 0 解锁上车 → 1 准备起步 → 2 挂D起步 → 3 强力超车 → 4 能量回收
//   → 5 FSD巡航 → 6 倒车 → 7 开门下车 → 8 插枪充电
// 时间轴 1030 帧循环，每 CFG_MOCK_TICK_MS 推进一帧。
//
// 输出与真实 BLE 走同一条路径：构造 TeslaPayload -> applyTesla() -> g_vehicle，
// 因此 UI 代码无法分辨数据来自模拟器还是剧本（切换零成本）。
// ---------------------------------------------------------------------------
class MockSource {
 public:
  void begin();
  void loop();

 private:
  uint32_t m_frame = 0;
  uint32_t m_lastTick = 0;
};
