#pragma once

#include <Arduino.h>

#include "ble/BleLink.h"
#include "config.h"
#include "data/VehicleData.h"
#include "obd/Pids.h"

// ---------------------------------------------------------------------------
// ObdClient：OBD / ELM327 应用层
//
// 状态机：WaitLink -> Init(AT 初始化) -> Poll -> WaitResponse -> Poll ...
// 数据流：BLE 字节 -> feed() -> 按 \r \n > 切行 -> 解析 -> 写入 g_vehicle
// ---------------------------------------------------------------------------
class ObdClient {
 public:
  void begin(BleLink* link);
  void loop();

  // 由 BleLink 的 RxHandler 调用，喂入原始字节
  void feed(const uint8_t* data, size_t len);

  bool ready() const {
    return m_state == State::Poll || m_state == State::WaitResponse;
  }

 private:
  enum class State : uint8_t { WaitLink, Init, Poll, WaitResponse };

  void sendInitCmd();
  void sendRequest(const ObdPid& pid);
  void handleLine(const char* line);
  void handlePrompt();
  bool parseResponse(const char* line);
  void applyValue(ObdMetric metric, const uint8_t* bytes, size_t count);

  BleLink* m_link = nullptr;
  State m_state = State::WaitLink;

  // 收包组行
  char m_line[CFG_OBD_LINE_MAX];
  size_t m_lineLen = 0;

  // Init 阶段
  uint8_t m_initIdx = 0;
  uint32_t m_initSentAt = 0;

  // Poll 阶段
  uint8_t m_pidIdx = 0;
  uint32_t m_nextPollAt = 0;

  // 当前等待中的请求
  uint8_t m_pendingMode = 0;
  uint8_t m_pendingPid = 0;
  ObdMetric m_pendingMetric = ObdMetric::Speed;
  uint32_t m_requestAt = 0;
};
