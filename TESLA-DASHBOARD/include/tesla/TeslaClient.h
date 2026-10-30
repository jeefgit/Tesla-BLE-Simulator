#pragma once

#include <Arduino.h>

#include "ble/BleLink.h"
#include "config.h"
#include "data/VehicleData.h"
#include "tesla/TeslaCrypto.h"
#include "tesla/TeslaPayload.h"

// ---------------------------------------------------------------------------
// TeslaClient：特斯拉 BLE 应用层
//
// 双路径：
//   1) 加密路径（CFG_TESLA_CRYPTO=1）：连接后必须先完成握手，握手成功后才
//      周期发 GetVehicleData 并解密 CarServer.Response；握手前不解析任何
//      53B 旁路帧，ready() 恒为 false（仪表降级显示）
//   2) 旁路路径（仅 CFG_TESLA_CRYPTO=0）：模拟器/旧固件直接推送 53 字节
//      SimPayload（帧长识别）-> applyTesla() 写入 g_vehicle
//
// 加密模式的分流规则：先看 2 字节大端长度前缀是否自洽
// （declared + 2 == 已收字节数），自洽即视为加密帧。
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

  // 加密会话是否已建立（仪表状态页可用来区分"旁路/加密"）
  bool encrypted() const { return m_crypto.hasSession(); }

  // 特斯拉广播名 = "S" + sha1(VIN) 前16位hex + "C"（与 sim_server.py 规则一致）
  static String advertisedNameFromVin(const char* vin);

 private:
  static constexpr size_t kFrame = sizeof(TeslaPayload);

  bool plausible(const TeslaPayload& p) const;
  void consume(size_t n);

  // 从接收缓冲中按"长度前缀/53B 帧"分流解析
  void processBuffer();

  // 连接生命周期与命令调度
  void onConnected();
  void onDisconnected();
  void sendHandshake();
  void sendPoll();
  void sendPairingRequest();

  // TeslaCrypto 回调
  void onCryptoResponse(const uint8_t* plaintext, size_t len, uint32_t fault);
  void onCryptoSession(bool ok);
  void onNotPaired();
  void onPairingStatus(uint32_t operationStatus, uint32_t info, bool terminal);

  static bool due(uint32_t at) { return (int32_t)(millis() - at) >= 0; }

  // 解析 CarServer.Response（已解密的明文）
  bool decodeVehicleData(const uint8_t* data, size_t len);

  BleLink* m_link = nullptr;

  // 组帧缓冲：同时容纳加密帧（长度前缀 + RoutableMessage）与 53B 旁路帧
  uint8_t m_buf[CFG_TESLA_MAX_FRAME + 2];
  size_t m_len = 0;

  TeslaCrypto m_crypto;
  // 发送缓冲：握手请求/加密命令最长不超过一帧上限
  uint8_t m_tx[CFG_TESLA_MAX_FRAME];

  bool m_wasConnected = false;
  uint32_t m_nextActionAt = 0;
  uint32_t m_handshakeTries = 0;

  // 配钥（add-key-request）状态
  bool m_notPaired = false;       // 车辆回 KEY_NOT_ON_WHITELIST
  bool m_pairDone = false;        // 已收到配钥终态
  bool m_pairWaitLogged = false;  // WAIT 日志只打一次
  uint32_t m_pairSends = 0;
  uint32_t m_nextPairAt = 0;

  uint32_t m_packets = 0;
  uint32_t m_lastPacketAt = 0;
  uint32_t m_droppedBytes = 0;
  bool m_timeoutLogged = false;
  bool m_decodeLogged = false;
  bool m_bypassIgnoredLogged = false;
};
