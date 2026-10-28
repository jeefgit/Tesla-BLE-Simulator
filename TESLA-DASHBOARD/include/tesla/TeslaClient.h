#pragma once

#include <Arduino.h>

#include "ble/BleLink.h"
#include "data/VehicleData.h"
#include "tesla/TeslaPayload.h"

// ---------------------------------------------------------------------------
// TeslaClient：特斯拉 BLE 应用层
//
// 数据流：BleLink 字节 -> feed() -> 53B 组帧(带逐字节重同步) ->
//         合理性校验 -> applyTesla() 写入 g_vehicle
//
// 特斯拉是"车端推送"模式（800ms 节拍 notify），与 OBD 的
// "请求-应答"轮询不同，本层没有发送逻辑，纯被动接收。
// ---------------------------------------------------------------------------
class TeslaClient {
 public:
  void begin(BleLink* link);
  void loop();

  // 由 BleLink 的 RxHandler 调用，喂入原始字节
  void feed(const uint8_t* data, size_t len);

  // 最近是否持续收到数据（超时则 false，仪表降级显示）
  bool ready() const;
  uint32_t packetCount() const { return m_packets; }
  uint32_t lastPacketMs() const { return m_lastPacketAt; }

  // 特斯拉广播名 = "S" + sha1(VIN) 前16位hex + "C"（与 sim_server.py 规则一致）
  static String advertisedNameFromVin(const char* vin);

 private:
  static constexpr size_t kFrame = sizeof(TeslaPayload);

  bool plausible(const TeslaPayload& p) const;
  void consume(size_t n);

  BleLink* m_link = nullptr;

  // 组帧缓冲：BLE 通知可能被 MTU 拆分，也可能一次到完整帧
  uint8_t m_buf[kFrame];
  size_t m_len = 0;

  uint32_t m_packets = 0;
  uint32_t m_lastPacketAt = 0;
  uint32_t m_droppedBytes = 0;
  bool m_timeoutLogged = false;
};
