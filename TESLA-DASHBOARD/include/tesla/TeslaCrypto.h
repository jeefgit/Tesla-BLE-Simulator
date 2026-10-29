#pragma once

#include <Arduino.h>

#include "config.h"

#include <functional>
#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// TeslaCrypto：特斯拉车端加密协议客户端（ESP32 侧）
//
// 负责：
//   1. NIST P-256 ECDH 密钥协商（客户端私钥 -> 公钥 -> 与车端公钥求共享点）
//   2. 会话密钥派生
//        K  = SHA1(shared_x)[:16]
//        SESSION_INFO_KEY = HMAC-SHA256(K, "session info")
//   3. 握手：SessionInfoRequest -> 校验 SessionInfo + HMAC
//   4. 加密命令：AES-128-GCM Personalized，元数据 TLV 的 SHA256 作为 AAD
//   5. 解密响应：AES-GCM Response，AAD 绑定请求 tag
//
// 所有跨 BLE 的帧均为「2 字节大端长度 + RoutableMessage」。
// ---------------------------------------------------------------------------
class TeslaCrypto {
 public:
  // 解密后的业务负载（CarServer.Response / VCSEC.FromVCSECMessage）
  using ResponseHandler =
      std::function<void(const uint8_t* plaintext, size_t len, uint32_t fault)>;
  // 握手结果回调（true = 会话建立）
  using SessionHandler = std::function<void(bool ok)>;

  void begin(const String& vin);
  void reset();

  bool hasSession() const { return m_hasSession; }

  void setResponseHandler(ResponseHandler h) { m_onResponse = std::move(h); }
  void setSessionHandler(SessionHandler h) { m_onSession = std::move(h); }

  // 构造握手请求帧（含 2 字节长度前缀），写入 out
  bool buildHandshakeRequest(uint8_t* out, size_t cap, size_t* outLen);

  // 构造 GetVehicleData 加密命令帧（含 2 字节长度前缀）
  bool buildGetVehicleData(uint8_t* out, size_t cap, size_t* outLen);

  // 喂入一个已去掉长度前缀的 RoutableMessage 负载
  void handleFrame(const uint8_t* payload, size_t len);

  uint32_t commandsSent() const { return m_commandsSent; }
  uint32_t responsesOk() const { return m_responsesOk; }
  uint32_t lastFault() const { return m_lastFault; }

 private:
  bool ensureKeys();
  bool handleHandshake(const uint8_t* sessionInfo, size_t len,
                       const uint8_t* tag, size_t tagLen);
  bool handleResponse(const uint8_t* ciphertext, size_t len,
                      const uint8_t* nonce, size_t nonceLen, uint32_t counter,
                      const uint8_t* tag, size_t tagLen, uint32_t flags,
                      uint32_t fault);
  void report(const uint8_t* plaintext, size_t len, uint32_t fault);
  uint32_t domainNow() const;
  uint32_t commandExpiry() const;

  String m_vin;
  bool m_keysReady = false;
  uint8_t m_priv[32] = {0};
  uint8_t m_pub[65] = {0};
  uint8_t m_routing[16] = {0};

  bool m_hasSession = false;
  uint8_t m_sharedKey[16] = {0};
  uint8_t m_sessionInfoKey[32] = {0};
  uint8_t m_epoch[16] = {0};
  uint8_t m_challenge[16] = {0};

  uint32_t m_counter = 0;
  uint32_t m_clockBase = 0;
  uint32_t m_clockBaseMs = 0;

  uint8_t m_lastReqTag[16] = {0};
  uint8_t m_lastReqUuid[16] = {0};
  uint32_t m_lastReqCounter = 0;
  uint32_t m_lastRespCounter = 0;
  uint32_t m_lastFault = 0;

  uint32_t m_commandsSent = 0;
  uint32_t m_responsesOk = 0;

  // 解密输出缓冲：响应明文最大不超过一帧上限
  uint8_t m_plain[CFG_TESLA_MAX_FRAME] = {0};

  ResponseHandler m_onResponse;
  SessionHandler m_onSession;
};
