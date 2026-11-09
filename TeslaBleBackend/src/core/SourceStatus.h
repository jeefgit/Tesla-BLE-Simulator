#pragma once

#include "core/LinkState.h"
#include "core/PairingHint.h"

// ---------------------------------------------------------------------------
// SourceStatus：数据源的运行时状态快照
//
// UI 用它决定显示「扫描 / 连接 / 同步 / 断连 / 配钥」等页面，
// 不关心底层是 BLE、OBD 还是演示剧本。
// ---------------------------------------------------------------------------
struct SourceStatus {
  LinkState link = LinkState::Idle;
  bool dataReady = false;    // 最近是否持续收到有效数据（用于日志/诊断）
  bool encrypted = false;    // 加密会话是否已建立（BLE 专属，其他源恒为 true）
  PairingHint pairing{};
  uint32_t lastPacketMs = 0; // 最近一帧数据的 millis()
};
