#include "tesla/TeslaCrypto.h"

#include <esp_random.h>
#include <mbedtls/bignum.h>
#include <mbedtls/ecp.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <mbedtls/sha1.h>
#include <mbedtls/sha256.h>
#include <string.h>

#include "tesla/PbCodec.h"

// ---------------------------------------------------------------------------
// 特斯拉 BLE 加密协议客户端实现（ESP32 侧）
//
// 与模拟器 tesla_protocol/ 逐字节对齐：
//   identity.py  -> NIST P-256 ECDH
//   metadata.py  -> TLV 元数据（按 tag 升序 + 0xFF 结尾）
//   session.py   -> K = SHA1(Sx)[:16]、HMAC-SHA256(K,"session info")
//   secure_link.py -> 2 字节大端帧、AES-128-GCM（AAD = SHA256(TLV)）
//   server.py    -> RoutableMessage / SessionInfo / CarServer.Action
//
// 所有常量均取自 vendored 的官方 .proto，改协议前先对照 .proto。
// ---------------------------------------------------------------------------
namespace {

// UniversalMessage.Domain
constexpr uint32_t kDomainInfotainment = 3;
constexpr uint32_t kDomainVcsec = 2;

// VCSEC.SignatureType：PRESENT_KEY 表示「需要本地实体（NFC 卡）授权」
constexpr uint32_t kSigPresentKey = 2;


// Signatures.SignatureType
constexpr uint8_t kSigAesGcmPersonalized = 5;
constexpr uint8_t kSigHmac = 6;
constexpr uint8_t kSigAesGcmResponse = 9;

// Signatures.Tag
constexpr uint8_t kTagSignatureType = 0;
constexpr uint8_t kTagDomain = 1;
constexpr uint8_t kTagPersonalization = 2;
constexpr uint8_t kTagEpoch = 3;
constexpr uint8_t kTagExpiresAt = 4;
constexpr uint8_t kTagCounter = 5;
constexpr uint8_t kTagChallenge = 6;
constexpr uint8_t kTagFlags = 7;
constexpr uint8_t kTagRequestHash = 8;
constexpr uint8_t kTagFault = 9;
constexpr uint8_t kTagEnd = 0xFF;

// UniversalMessage.Flags：FLAG_ENCRYPT_RESPONSE 位号为 1
constexpr uint32_t kFlagEncryptResponse = 1u << 1;

// 仪表只走 Infotainment（遥测/充电/空调/门锁状态都在这一域）
constexpr uint32_t kCommandDomain = kDomainInfotainment;

// UniversalMessage.MessageFault_E
constexpr uint32_t kFaultNone = 0;
constexpr uint32_t kFaultInvalidSignature = 5;
constexpr uint32_t kFaultInvalidDomains = 8;

constexpr size_t kTlvCap = 128;

// ── 随机数 ─────────────────────────────────────────────────────────
void fillRandom(uint8_t* out, size_t len) {
  for (size_t i = 0; i < len; i += 4) {
    const uint32_t r = esp_random();
    const size_t n = (len - i) < 4 ? (len - i) : 4;
    memcpy(out + i, &r, n);
  }
}

int rngCallback(void*, unsigned char* buf, size_t len) {
  fillRandom(buf, len);
  return 0;
}

// ── 摘要 ─────────────────────────────────────────────────────────
bool sha1Bytes(const uint8_t* in, size_t len, uint8_t out[20]) {
  return mbedtls_sha1(in, len, out) == 0;
}

bool sha256Bytes(const uint8_t* in, size_t len, uint8_t out[32]) {
  return mbedtls_sha256(in, len, out, 0) == 0;
}

bool hmacSha256(const uint8_t* key, size_t keyLen, const uint8_t* data,
                size_t dataLen, uint8_t out[32]) {
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!info) return false;
  return mbedtls_md_hmac(info, key, keyLen, data, dataLen, out) == 0;
}

// ── AES-128-GCM ────────────────────────────────────────────────────
bool gcmEncrypt(const uint8_t key[16], const uint8_t nonce[12],
                const uint8_t* aad, size_t aadLen, const uint8_t* in,
                size_t inLen, uint8_t* out, uint8_t tag[16]) {
  mbedtls_gcm_context ctx;
  mbedtls_gcm_init(&ctx);
  int rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 128);
  if (rc == 0) {
    rc = mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT, inLen, nonce, 12,
                                   aad, aadLen, in, out, 16, tag);
  }
  mbedtls_gcm_free(&ctx);
  return rc == 0;
}

bool gcmDecrypt(const uint8_t key[16], const uint8_t nonce[12],
                const uint8_t* aad, size_t aadLen, const uint8_t* in,
                size_t inLen, const uint8_t tag[16], uint8_t* out) {
  mbedtls_gcm_context ctx;
  mbedtls_gcm_init(&ctx);
  int rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 128);
  if (rc == 0) {
    rc = mbedtls_gcm_auth_decrypt(&ctx, inLen, nonce, 12, aad, aadLen, tag, 16,
                                  in, out);
  }
  mbedtls_gcm_free(&ctx);
  return rc == 0;
}

// ── TLV 元数据 ────────────────────────────────────────────────────
// 调用方必须按 tag 升序 put（本文件的构造顺序天然满足）。
struct TlvBuilder {
  uint8_t buf[kTlvCap];
  size_t len = 0;
  bool ok = true;

  void put(uint8_t tag, const uint8_t* value, uint8_t n) {
    if (!ok) return;
    if (len + 2 + n + 1 > kTlvCap) {
      ok = false;
      return;
    }
    buf[len++] = tag;
    buf[len++] = n;
    if (n) {
      memcpy(buf + len, value, n);
      len += n;
    }
  }

  void putU8(uint8_t tag, uint8_t v) { put(tag, &v, 1); }

  void putU32(uint8_t tag, uint32_t v) {
    const uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16),
                          (uint8_t)(v >> 8), (uint8_t)v};
    put(tag, b, 4);
  }

  void end() {
    if (!ok) return;
    if (len + 1 > kTlvCap) {
      ok = false;
      return;
    }
    buf[len++] = kTagEnd;
  }

  const uint8_t* data() const { return buf; }
  size_t size() const { return len; }
};

// 握手响应元数据：HMAC + VIN + challenge（模拟器 metadata.session_info_metadata）
void buildSessionInfoMetadata(TlvBuilder& t, const char* vin,
                              const uint8_t* challenge, size_t challengeLen) {
  t.putU8(kTagSignatureType, kSigHmac);
  t.put(kTagPersonalization, (const uint8_t*)vin, (uint8_t)strlen(vin));
  if (challenge && challengeLen == 16) {
    t.put(kTagChallenge, challenge, 16);
  }
  t.end();
}

// 命令请求元数据（模拟器 metadata.request_metadata）
void buildRequestMetadata(TlvBuilder& t, const char* vin, const uint8_t* epoch,
                          uint32_t counter, uint32_t expiresAt, uint32_t flags) {
  t.putU8(kTagSignatureType, kSigAesGcmPersonalized);
  t.putU8(kTagDomain, (uint8_t)kCommandDomain);
  t.put(kTagPersonalization, (const uint8_t*)vin, (uint8_t)strlen(vin));
  if (epoch) t.put(kTagEpoch, epoch, 16);
  if (expiresAt) t.putU32(kTagExpiresAt, expiresAt);
  t.putU32(kTagCounter, counter);
  if (flags) t.putU32(kTagFlags, flags);
  t.end();
}

// 响应元数据（模拟器 metadata.response_metadata，FLAGS/FAULT 始终写入）
void buildResponseMetadata(TlvBuilder& t, const char* vin, uint32_t counter,
                           uint32_t flags, const uint8_t* requestHash,
                           size_t requestHashLen, uint32_t fault) {
  t.putU8(kTagSignatureType, kSigAesGcmResponse);
  t.putU8(kTagDomain, (uint8_t)kCommandDomain);
  t.put(kTagPersonalization, (const uint8_t*)vin, (uint8_t)strlen(vin));
  t.putU32(kTagCounter, counter);
  t.putU32(kTagFlags, flags);
  if (requestHash && requestHashLen) {
    t.put(kTagRequestHash, requestHash, (uint8_t)requestHashLen);
  }
  t.putU32(kTagFault, fault);
  t.end();
}

// ── ECDH（NIST P-256）──────────────────────────────────────────────
bool derivePublic(const uint8_t priv[32], uint8_t pub[65]) {
  mbedtls_ecp_group grp;
  mbedtls_mpi d;
  mbedtls_ecp_point q;
  mbedtls_ecp_group_init(&grp);
  mbedtls_mpi_init(&d);
  mbedtls_ecp_point_init(&q);

  bool ok = false;
  do {
    if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) != 0) break;
    if (mbedtls_mpi_read_binary(&d, priv, 32) != 0) break;
    if (mbedtls_ecp_check_privkey(&grp, &d) != 0) break;
    if (mbedtls_ecp_mul(&grp, &q, &d, &grp.G, rngCallback, nullptr) != 0) {
      break;
    }
    size_t olen = 0;
    if (mbedtls_ecp_point_write_binary(&grp, &q,
                                       MBEDTLS_ECP_PF_UNCOMPRESSED, &olen, pub,
                                       65) != 0) {
      break;
    }
    if (olen != 65) break;
    ok = true;
  } while (0);

  mbedtls_ecp_point_free(&q);
  mbedtls_mpi_free(&d);
  mbedtls_ecp_group_free(&grp);
  return ok;
}

bool ecdhSharedX(const uint8_t priv[32], const uint8_t peerPub[65],
                 uint8_t sharedX[32]) {
  mbedtls_ecp_group grp;
  mbedtls_mpi d;
  mbedtls_ecp_point peer;
  mbedtls_ecp_point shared;
  mbedtls_ecp_group_init(&grp);
  mbedtls_mpi_init(&d);
  mbedtls_ecp_point_init(&peer);
  mbedtls_ecp_point_init(&shared);

  bool ok = false;
  do {
    if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) != 0) break;
    if (mbedtls_ecp_point_read_binary(&grp, &peer, peerPub, 65) != 0) break;
    if (mbedtls_ecp_check_pubkey(&grp, &peer) != 0) break;
    if (mbedtls_mpi_read_binary(&d, priv, 32) != 0) break;
    if (mbedtls_ecp_check_privkey(&grp, &d) != 0) break;
    if (mbedtls_ecp_mul(&grp, &shared, &d, &peer, rngCallback, nullptr) != 0) {
      break;
    }
    // mbedtls 3.x 的 ecp_point 坐标是私有成员（MBEDTLS_PRIVATE），
    // 只能通过 write_binary 导出：65B = 0x04 + X(32) + Y(32)，取 X 即可。
    uint8_t sharedPt[65];
    size_t olen = 0;
    if (mbedtls_ecp_point_write_binary(&grp, &shared,
                                       MBEDTLS_ECP_PF_UNCOMPRESSED, &olen,
                                       sharedPt, sizeof(sharedPt)) != 0) {
      break;
    }
    if (olen != sizeof(sharedPt)) break;
    memcpy(sharedX, sharedPt + 1, 32);
    ok = true;
  } while (0);

  mbedtls_ecp_point_free(&shared);
  mbedtls_ecp_point_free(&peer);
  mbedtls_mpi_free(&d);
  mbedtls_ecp_group_free(&grp);
  return ok;
}

// ── hex ───────────────────────────────────────────────────────────
int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool parseHexKey(const char* hex, uint8_t out[32]) {
  if (!hex) return false;
  if (strlen(hex) != 64) return false;
  for (size_t i = 0; i < 32; ++i) {
    const int hi = hexNibble(hex[i * 2]);
    const int lo = hexNibble(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

// ── RoutableMessage 字段解析辅助 ─────────────────────────────────
void parseMessageStatus(pb::Reader& r, uint32_t* fault) {
  while (!r.atEnd()) {
    uint32_t f = 0, w = 0;
    if (!r.next(&f, &w)) break;
    if (f == 2 && w == pb::WIRE_VARINT) {
      uint64_t v = 0;
      if (r.readVarint(&v)) *fault = (uint32_t)v;
    } else {
      r.skip(w);
    }
  }
}

struct SignatureFields {
  const uint8_t* sessionInfoTag = nullptr;
  size_t sessionInfoTagLen = 0;
  const uint8_t* nonce = nullptr;
  size_t nonceLen = 0;
  const uint8_t* tag = nullptr;
  size_t tagLen = 0;
  uint32_t counter = 0;
  bool hasResponse = false;
};

void parseSignatureData(pb::Reader& r, SignatureFields* out) {
  while (!r.atEnd()) {
    uint32_t f = 0, w = 0;
    if (!r.next(&f, &w)) break;
    if (f == 6 && w == pb::WIRE_LEN) {
      pb::Reader sub;
      if (r.readMessage(&sub)) {
        while (!sub.atEnd()) {
          uint32_t sf = 0, sw = 0;
          if (!sub.next(&sf, &sw)) break;
          if (sf == 1 && sw == pb::WIRE_LEN) {
            const uint8_t* p = nullptr;
            size_t n = 0;
            if (sub.readBytes(&p, &n)) {
              out->sessionInfoTag = p;
              out->sessionInfoTagLen = n;
            }
          } else {
            sub.skip(sw);
          }
        }
      }
    } else if (f == 9 && w == pb::WIRE_LEN) {
      pb::Reader sub;
      if (r.readMessage(&sub)) {
        out->hasResponse = true;
        while (!sub.atEnd()) {
          uint32_t sf = 0, sw = 0;
          if (!sub.next(&sf, &sw)) break;
          if (sf == 1 && sw == pb::WIRE_LEN) {
            const uint8_t* p = nullptr;
            size_t n = 0;
            if (sub.readBytes(&p, &n)) {
              out->nonce = p;
              out->nonceLen = n;
            }
          } else if (sf == 2 && sw == pb::WIRE_VARINT) {
            uint64_t v = 0;
            if (sub.readVarint(&v)) out->counter = (uint32_t)v;
          } else if (sf == 3 && sw == pb::WIRE_LEN) {
            const uint8_t* p = nullptr;
            size_t n = 0;
            if (sub.readBytes(&p, &n)) {
              out->tag = p;
              out->tagLen = n;
            }
          } else {
            sub.skip(sw);
          }
        }
      }
    } else {
      r.skip(w);
    }
  }
}

// from_destination.domain（Destination.domain = 1）
uint32_t parseDestinationDomain(pb::Reader& r) {
  uint32_t domain = 0;
  while (!r.atEnd()) {
    uint32_t f = 0, w = 0;
    if (!r.next(&f, &w)) break;
    if (f == 1 && w == pb::WIRE_VARINT) {
      uint64_t v = 0;
      if (r.readVarint(&v)) domain = (uint32_t)v;
    } else {
      r.skip(w);
    }
  }
  return domain;
}

// 解析 FromVCSECMessage.commandStatus（配钥进度）
//   CommandStatus            : operationStatus=1, whitelistOperationStatus=3
//   WhitelistOperation_status: whitelistOperationInformation=1, operationStatus=3
// terminal=true 表示已收到 WhitelistOperation_status（配钥终态）。
bool parsePairingStatus(const uint8_t* data, size_t len, uint32_t* opStatus,
                        uint32_t* info, bool* terminal) {
  *opStatus = 0;
  *info = 0;
  *terminal = false;
  bool found = false;

  pb::Reader r(data, len);
  while (!r.atEnd()) {
    uint32_t f = 0, w = 0;
    if (!r.next(&f, &w)) break;
    if (f == 4 && w == pb::WIRE_LEN) {  // FromVCSECMessage.commandStatus
      pb::Reader cs;
      if (!r.readMessage(&cs)) break;
      found = true;
      while (!cs.atEnd()) {
        uint32_t cf = 0, cw = 0;
        if (!cs.next(&cf, &cw)) break;
        if (cf == 1 && cw == pb::WIRE_VARINT) {
          uint64_t v = 0;
          if (cs.readVarint(&v)) *opStatus = (uint32_t)v;
        } else if (cf == 3 && cw == pb::WIRE_LEN) {
          pb::Reader wl;
          if (!cs.readMessage(&wl)) break;
          *terminal = true;
          while (!wl.atEnd()) {
            uint32_t wf = 0, ww = 0;
            if (!wl.next(&wf, &ww)) break;
            if (wf == 1 && ww == pb::WIRE_VARINT) {
              uint64_t v = 0;
              if (wl.readVarint(&v)) *info = (uint32_t)v;
            } else {
              wl.skip(ww);
            }
          }
        } else {
          cs.skip(cw);
        }
      }
    } else {
      r.skip(w);
    }
  }
  return found;
}

}  // namespace

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------
void TeslaCrypto::begin(const String& vin) {
  m_vin = vin;
  reset();
}

void TeslaCrypto::reset() {
  m_hasSession = false;
  memset(m_sharedKey, 0, sizeof(m_sharedKey));
  memset(m_sessionInfoKey, 0, sizeof(m_sessionInfoKey));
  memset(m_epoch, 0, sizeof(m_epoch));
  memset(m_challenge, 0, sizeof(m_challenge));
  memset(m_lastReqTag, 0, sizeof(m_lastReqTag));
  memset(m_lastReqUuid, 0, sizeof(m_lastReqUuid));
  m_counter = 0;
  m_lastReqCounter = 0;
  m_lastRespCounter = 0;
  m_clockBase = 0;
  m_clockBaseMs = 0;
  m_lastFault = 0;
}

bool TeslaCrypto::ensureKeys() {
  if (m_keysReady) return true;

  uint8_t key[32] = {0};
  bool loaded = false;
#if defined(CFG_TESLA_CLIENT_KEY_HEX)
  loaded = parseHexKey(CFG_TESLA_CLIENT_KEY_HEX, key);
#endif

  mbedtls_ecp_group grp;
  mbedtls_mpi d;
  mbedtls_ecp_point q;
  mbedtls_ecp_group_init(&grp);
  mbedtls_mpi_init(&d);
  mbedtls_ecp_point_init(&q);

  bool ok = false;
  do {
    if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) != 0) break;

    if (!loaded) {
      // 未配置固定私钥：随机生成一个合法标量（1 <= d < n）
      for (int tries = 0; tries < 8; ++tries) {
        fillRandom(key, sizeof(key));
        if (mbedtls_mpi_read_binary(&d, key, sizeof(key)) == 0 &&
            mbedtls_ecp_check_privkey(&grp, &d) == 0) {
          loaded = true;
          break;
        }
      }
      if (!loaded) break;
    } else if (mbedtls_mpi_read_binary(&d, key, sizeof(key)) != 0 ||
               mbedtls_ecp_check_privkey(&grp, &d) != 0) {
      break;
    }

    memcpy(m_priv, key, sizeof(m_priv));

    if (mbedtls_ecp_mul(&grp, &q, &d, &grp.G, rngCallback, nullptr) != 0) break;
    size_t olen = 0;
    if (mbedtls_ecp_point_write_binary(&grp, &q,
                                       MBEDTLS_ECP_PF_UNCOMPRESSED, &olen, m_pub,
                                       sizeof(m_pub)) != 0) {
      break;
    }
    if (olen != sizeof(m_pub)) break;
    ok = true;
  } while (0);

  mbedtls_ecp_point_free(&q);
  mbedtls_mpi_free(&d);
  mbedtls_ecp_group_free(&grp);
  if (!ok) return false;

  fillRandom(m_routing, sizeof(m_routing));
  m_keysReady = true;

  // 完整公钥（0x04 || X || Y，65 字节 / 130 hex）就是需要登记到车机的配对公钥，
  // 串口一次性打印便于直接复制。
  char pubHex[132];
  for (size_t i = 0; i < sizeof(m_pub); ++i) {
    snprintf(pubHex + i * 2, 3, "%02x", m_pub[i]);
  }
  CFG_LOG("[CRYPTO] 客户端密钥就绪 pub=%s", pubHex);
  CFG_LOG("[CRYPTO] routing=%02x%02x%02x%02x", m_routing[0], m_routing[1],
          m_routing[2], m_routing[3]);
  return true;
}

uint32_t TeslaCrypto::domainNow() const {
  return m_clockBase + (uint32_t)((millis() - m_clockBaseMs) / 1000);
}

uint32_t TeslaCrypto::commandExpiry() const {
  return domainNow() + CFG_TESLA_COMMAND_TTL_S;
}

// ---------------------------------------------------------------------------
// 握手请求
// ---------------------------------------------------------------------------
bool TeslaCrypto::buildHandshakeRequest(uint8_t* out, size_t cap,
                                        size_t* outLen) {
  if (!out || !outLen) return false;
  if (!ensureKeys()) return false;

  fillRandom(m_challenge, sizeof(m_challenge));

  uint8_t destBuf[16];
  pb::Writer dest(destBuf, sizeof(destBuf));
  dest.putUint32(1, kCommandDomain);  // Destination.domain

  uint8_t fromBuf[32];
  pb::Writer from(fromBuf, sizeof(fromBuf));
  from.putBytes(2, m_routing, sizeof(m_routing));  // routing_address

  uint8_t sirBuf[80];
  pb::Writer sir(sirBuf, sizeof(sirBuf));
  sir.putBytes(1, m_pub, sizeof(m_pub));  // SessionInfoRequest.public_key

  uint8_t body[512];
  pb::Writer w(body, sizeof(body));
  w.putMessage(6, dest);   // to_destination
  w.putMessage(7, from);   // from_destination
  w.putMessage(14, sir);   // session_info_request
  w.putBytes(51, m_challenge, sizeof(m_challenge));  // uuid
  if (!dest.ok() || !from.ok() || !sir.ok() || !w.ok()) return false;

  const size_t n = w.size();
  if (cap < n + 2) return false;
  out[0] = (uint8_t)(n >> 8);
  out[1] = (uint8_t)(n & 0xFF);
  memcpy(out + 2, body, n);
  *outLen = n + 2;
  return true;
}

// ---------------------------------------------------------------------------
// add-key-request（未配对时的明文配钥信封）
//
// 官方 vehicle-command pkg/vehicle/security.go:
//   ToVCSECMessage{SignedMessage{ProtobufMessageAsBytes=UnsignedMessage{
//       WhitelistOperation{addKeyToWhitelistAndAddPermissions{key,keyRole},
//                          metadataForKey{keyFormFactor}}},
//     SignatureType=SIGNATURE_TYPE_PRESENT_KEY}}
// 直接 conn.Send(明文)，不经 ECDH 会话、无签名；车辆提示刷卡后回
// CommandStatus{operationStatus=WAIT}，刷卡确认后回终态 WhitelistOperation_status。
// ---------------------------------------------------------------------------
bool TeslaCrypto::buildPairingRequest(uint8_t* out, size_t cap,
                                      size_t* outLen) {
  if (!out || !outLen) return false;
  if (!ensureKeys()) return false;

  // PublicKey{PublicKeyRaw = m_pub}
  uint8_t keyBuf[80];
  pb::Writer key(keyBuf, sizeof(keyBuf));
  key.putBytes(1, m_pub, sizeof(m_pub));

  // PermissionChange{key, keyRole}
  uint8_t changeBuf[96];
  pb::Writer change(changeBuf, sizeof(changeBuf));
  change.putMessage(1, key);
  change.putUint32(4, CFG_TESLA_PAIR_ROLE);

  // KeyMetadata{keyFormFactor}
  uint8_t metaBuf[8];
  pb::Writer meta(metaBuf, sizeof(metaBuf));
  meta.putUint32(1, CFG_TESLA_PAIR_FORM_FACTOR);

  // WhitelistOperation{addKeyToWhitelistAndAddPermissions, metadataForKey}
  uint8_t opBuf[160];
  pb::Writer op(opBuf, sizeof(opBuf));
  op.putMessage(5, change);  // addKeyToWhitelistAndAddPermissions
  op.putMessage(6, meta);    // metadataForKey

  // UnsignedMessage{WhitelistOperation}
  uint8_t unsignedBuf[192];
  pb::Writer um(unsignedBuf, sizeof(unsignedBuf));
  um.putMessage(16, op);  // UnsignedMessage.WhitelistOperation

  // SignedMessage{protobufMessageAsBytes, signatureType}
  uint8_t signedBuf[256];
  pb::Writer sm(signedBuf, sizeof(signedBuf));
  sm.putBytes(2, um.data(), um.size());
  sm.putUint32(3, kSigPresentKey);

  // ToVCSECMessage{signedMessage}
  uint8_t body[320];
  pb::Writer w(body, sizeof(body));
  w.putMessage(1, sm);
  if (!key.ok() || !change.ok() || !meta.ok() || !op.ok() || !um.ok() ||
      !sm.ok() || !w.ok()) {
    CFG_LOG("[PAIR] add-key-request 编码失败（缓冲区不足）");
    return false;
  }

  const size_t n = w.size();
  if (cap < n + 2) return false;
  out[0] = (uint8_t)(n >> 8);
  out[1] = (uint8_t)(n & 0xFF);
  memcpy(out + 2, body, n);
  *outLen = n + 2;
  return true;
}

// ---------------------------------------------------------------------------
// GetVehicleData 加密命令
// ---------------------------------------------------------------------------
bool TeslaCrypto::buildGetVehicleData(uint8_t* out, size_t cap, size_t* outLen) {
  if (!out || !outLen) return false;
  if (!m_hasSession) return false;
  if (!ensureKeys()) return false;

  // CarServer.Action { vehicleAction { getVehicleData { ... } } }
  uint8_t gvdBuf[48];
  pb::Writer gvd(gvdBuf, sizeof(gvdBuf));
  gvd.putEmptyMessage(2);   // getChargeState
  gvd.putEmptyMessage(3);   // getClimateState
  gvd.putEmptyMessage(4);   // getDriveState
  gvd.putEmptyMessage(8);   // getClosuresState
  gvd.putEmptyMessage(14);  // getTirePressureState

  uint8_t vaBuf[64];
  pb::Writer va(vaBuf, sizeof(vaBuf));
  va.putMessage(1, gvd);  // VehicleAction.getVehicleData

  uint8_t actionBuf[80];
  pb::Writer action(actionBuf, sizeof(actionBuf));
  action.putMessage(2, va);  // Action.vehicleAction
  if (!gvd.ok() || !va.ok() || !action.ok()) return false;

  const size_t ptLen = action.size();
  if (ptLen == 0 || ptLen > 96) return false;

  const uint32_t counter = m_counter + 1;
  const uint32_t expiresAt = commandExpiry();
  const uint32_t flags = kFlagEncryptResponse;

  TlvBuilder meta;
  buildRequestMetadata(meta, m_vin.c_str(), m_epoch, counter, expiresAt, flags);
  if (!meta.ok) return false;

  uint8_t aad[32];
  if (!sha256Bytes(meta.data(), meta.size(), aad)) return false;

  uint8_t nonce[12];
  fillRandom(nonce, sizeof(nonce));
  uint8_t ciphertext[96];
  uint8_t tag[16];
  if (!gcmEncrypt(m_sharedKey, nonce, aad, sizeof(aad), action.data(), ptLen,
                  ciphertext, tag)) {
    CFG_LOG("[CRYPTO] AES-GCM 加密失败");
    return false;
  }

  uint8_t uuid[16];
  fillRandom(uuid, sizeof(uuid));

  uint8_t destBuf[16];
  pb::Writer dest(destBuf, sizeof(destBuf));
  dest.putUint32(1, kCommandDomain);

  uint8_t fromBuf[32];
  pb::Writer from(fromBuf, sizeof(fromBuf));
  from.putBytes(2, m_routing, sizeof(m_routing));

  uint8_t kidBuf[80];
  pb::Writer kid(kidBuf, sizeof(kidBuf));
  kid.putBytes(1, m_pub, sizeof(m_pub));  // signer_identity.public_key

  uint8_t gcmBuf[96];
  pb::Writer gcm(gcmBuf, sizeof(gcmBuf));
  gcm.putBytes(1, m_epoch, sizeof(m_epoch));
  gcm.putBytes(2, nonce, sizeof(nonce));
  gcm.putUint32(3, counter);
  gcm.putFixed32(4, expiresAt);
  gcm.putBytes(5, tag, sizeof(tag));

  uint8_t sigBuf[160];
  pb::Writer sig(sigBuf, sizeof(sigBuf));
  sig.putMessage(1, kid);   // signer_identity
  sig.putMessage(5, gcm);   // AES_GCM_Personalized_data

  uint8_t body[256];
  pb::Writer w(body, sizeof(body));
  w.putMessage(6, dest);             // to_destination
  w.putMessage(7, from);             // from_destination
  w.putBytes(10, ciphertext, ptLen); // protobuf_message_as_bytes
  w.putMessage(13, sig);             // signature_data
  w.putUint32(52, flags);            // flags
  w.putBytes(51, uuid, sizeof(uuid));// uuid
  if (!dest.ok() || !from.ok() || !kid.ok() || !gcm.ok() || !sig.ok() ||
      !w.ok()) {
    return false;
  }

  const size_t n = w.size();
  if (cap < n + 2) return false;
  out[0] = (uint8_t)(n >> 8);
  out[1] = (uint8_t)(n & 0xFF);
  memcpy(out + 2, body, n);
  *outLen = n + 2;

  memcpy(m_lastReqTag, tag, sizeof(m_lastReqTag));
  memcpy(m_lastReqUuid, uuid, sizeof(m_lastReqUuid));
  m_lastReqCounter = counter;
  m_counter = counter;
  ++m_commandsSent;
  return true;
}

// ---------------------------------------------------------------------------
// 入站帧
// ---------------------------------------------------------------------------
void TeslaCrypto::handleFrame(const uint8_t* payload, size_t len) {
  if (!payload || len == 0) return;

  const uint8_t* sessionInfo = nullptr;
  size_t sessionInfoLen = 0;
  const uint8_t* ciphertext = nullptr;
  size_t ciphertextLen = 0;
  uint32_t flags = 0;
  uint32_t fault = 0;
  uint32_t fromDomain = 0;
  SignatureFields sig;

  pb::Reader r(payload, len);
  while (!r.atEnd()) {
    uint32_t f = 0, w = 0;
    if (!r.next(&f, &w)) break;
    switch (f) {
      case 7:  // from_destination
        if (w == pb::WIRE_LEN) {
          pb::Reader d;
          if (r.readMessage(&d)) fromDomain = parseDestinationDomain(d);
        } else {
          r.skip(w);
        }
        break;

      case 10:  // protobuf_message_as_bytes
        if (w == pb::WIRE_LEN) {
          const uint8_t* p = nullptr;
          size_t n = 0;
          if (r.readBytes(&p, &n)) {
            ciphertext = p;
            ciphertextLen = n;
          }
        } else {
          r.skip(w);
        }
        break;

      case 12:  // signedMessageStatus
        if (w == pb::WIRE_LEN) {
          pb::Reader st;
          if (r.readMessage(&st)) parseMessageStatus(st, &fault);
        } else {
          r.skip(w);
        }
        break;

      case 13:  // signature_data
        if (w == pb::WIRE_LEN) {
          pb::Reader sd;
          if (r.readMessage(&sd)) parseSignatureData(sd, &sig);
        } else {
          r.skip(w);
        }
        break;

      case 15:  // session_info
        if (w == pb::WIRE_LEN) {
          const uint8_t* p = nullptr;
          size_t n = 0;
          if (r.readBytes(&p, &n)) {
            sessionInfo = p;
            sessionInfoLen = n;
          }
        } else {
          r.skip(w);
        }
        break;

      case 52:  // flags
        if (w == pb::WIRE_VARINT) {
          uint64_t v = 0;
          if (r.readVarint(&v)) flags = (uint32_t)v;
        } else {
          r.skip(w);
        }
        break;

      default:
        r.skip(w);
        break;
    }
  }

  if (sessionInfo && sessionInfoLen) {
    handleHandshake(sessionInfo, sessionInfoLen, sig.sessionInfoTag,
                    sig.sessionInfoTagLen);
    return;
  }

  if (ciphertext && ciphertextLen) {
    // 未握手成功时不存在可用密钥；但 VCSEC 明文配钥状态必须放行，
    // 否则无法完成 add-key-request -> 刷卡 -> 终态 的引导流程。
    if (!m_hasSession) {
      if (fromDomain == kDomainVcsec) {
        uint32_t opStatus = 0;
        uint32_t info = 0;
        bool terminal = false;
        if (parsePairingStatus(ciphertext, ciphertextLen, &opStatus, &info,
                               &terminal)) {
          CFG_LOG("[PAIR] 配钥状态 operationStatus=%u info=%u%s",
                  (unsigned)opStatus, (unsigned)info,
                  terminal ? "（终态）" : "（等待刷卡）");
          if (m_onPairing) m_onPairing(opStatus, info, terminal);
          return;
        }
      }
      CFG_LOG("[CRYPTO] 未握手 RX %uB fromDomain=%u（非配钥状态，忽略）",
              (unsigned)ciphertextLen, (unsigned)fromDomain);
      return;
    }
    if (sig.hasResponse) {
      handleResponse(ciphertext, ciphertextLen, sig.nonce, sig.nonceLen,
                     sig.counter, sig.tag, sig.tagLen, flags, fault);
    } else if (fromDomain == kDomainVcsec) {
      // VCSEC 明文：可能是配钥终态（刷卡确认晚于重新握手时）
      uint32_t opStatus = 0;
      uint32_t info = 0;
      bool terminal = false;
      if (parsePairingStatus(ciphertext, ciphertextLen, &opStatus, &info,
                             &terminal)) {
        CFG_LOG("[PAIR] 配钥状态 operationStatus=%u info=%u%s",
                (unsigned)opStatus, (unsigned)info,
                terminal ? "（终态）" : "（等待刷卡）");
        if (m_onPairing) m_onPairing(opStatus, info, terminal);
      }
    } else {
      // 老固件忽略 FLAG_ENCRYPT_RESPONSE：负载直接是明文 CarServer.Response
      m_lastFault = fault;
      ++m_responsesOk;
      report(ciphertext, ciphertextLen, fault);
    }
    return;
  }

  if (fault != kFaultNone) {
    m_lastFault = fault;
    CFG_LOG("[CRYPTO] 协议层错误响应 fault=%u", (unsigned)fault);
    report(nullptr, 0, fault);
  }
}

bool TeslaCrypto::handleHandshake(const uint8_t* sessionInfo, size_t len,
                                  const uint8_t* tag, size_t tagLen) {
  uint32_t counter = 0;
  uint32_t clockTime = 0;
  uint32_t status = 0;
  const uint8_t* vehiclePub = nullptr;
  size_t vehiclePubLen = 0;
  const uint8_t* epoch = nullptr;
  size_t epochLen = 0;

  pb::Reader r(sessionInfo, len);
  while (!r.atEnd()) {
    uint32_t f = 0, w = 0;
    if (!r.next(&f, &w)) break;
    if (f == 1 && w == pb::WIRE_VARINT) {
      uint64_t v = 0;
      if (r.readVarint(&v)) counter = (uint32_t)v;
    } else if (f == 2 && w == pb::WIRE_LEN) {
      const uint8_t* p = nullptr;
      size_t n = 0;
      if (r.readBytes(&p, &n)) {
        vehiclePub = p;
        vehiclePubLen = n;
      }
    } else if (f == 3 && w == pb::WIRE_LEN) {
      const uint8_t* p = nullptr;
      size_t n = 0;
      if (r.readBytes(&p, &n)) {
        epoch = p;
        epochLen = n;
      }
    } else if (f == 4 && w == pb::WIRE_FIXED32) {
      uint32_t v = 0;
      if (r.readFixed32(&v)) clockTime = v;
    } else if (f == 5 && w == pb::WIRE_VARINT) {
      uint64_t v = 0;
      if (r.readVarint(&v)) status = (uint32_t)v;
    } else {
      r.skip(w);
    }
  }

  if (!vehiclePub || vehiclePubLen != 65 || vehiclePub[0] != 0x04) {
    CFG_LOG("[CRYPTO] 握手响应车辆公钥非法 (%uB)", (unsigned)vehiclePubLen);
    if (m_onSession) m_onSession(false);
    return false;
  }
  if (!epoch || epochLen != 16) {
    CFG_LOG("[CRYPTO] 握手响应 epoch 非法");
    if (m_onSession) m_onSession(false);
    return false;
  }
  if (status == 1) {
    // SESSION_INFO_STATUS_KEY_NOT_ON_WHITELIST：官方客户端映射为 ErrKeyNotPaired
    CFG_LOG("[CRYPTO] 车辆回 KEY_NOT_ON_WHITELIST（本机公钥未配对）");
    if (m_onNotPaired) m_onNotPaired();
    if (m_onSession) m_onSession(false);
    return false;
  }
  if (status != 0) {
    CFG_LOG("[CRYPTO] 握手被拒 status=%u", (unsigned)status);
    if (m_onSession) m_onSession(false);
    return false;
  }
  if (!tag || tagLen != 32) {
    CFG_LOG("[CRYPTO] 握手响应缺少 session_info_tag");
    if (m_onSession) m_onSession(false);
    return false;
  }

  uint8_t sharedX[32];
  if (!ecdhSharedX(m_priv, vehiclePub, sharedX)) {
    CFG_LOG("[CRYPTO] ECDH 失败");
    if (m_onSession) m_onSession(false);
    return false;
  }

  // K = SHA1(BIG_ENDIAN(Sx,32))[:16]
  uint8_t sha[20];
  if (!sha1Bytes(sharedX, sizeof(sharedX), sha)) {
    if (m_onSession) m_onSession(false);
    return false;
  }
  memcpy(m_sharedKey, sha, sizeof(m_sharedKey));

  // SESSION_INFO_KEY = HMAC-SHA256(K, "session info")
  static const uint8_t kLabel[] = "session info";
  if (!hmacSha256(m_sharedKey, sizeof(m_sharedKey), kLabel, sizeof(kLabel) - 1,
                  m_sessionInfoKey)) {
    if (m_onSession) m_onSession(false);
    return false;
  }

  TlvBuilder meta;
  buildSessionInfoMetadata(meta, m_vin.c_str(), m_challenge,
                           sizeof(m_challenge));
  if (!meta.ok) {
    if (m_onSession) m_onSession(false);
    return false;
  }

  uint8_t hmacInput[kTlvCap + 256];
  if (meta.size() + len > sizeof(hmacInput)) {
    CFG_LOG("[CRYPTO] 握手响应过大");
    if (m_onSession) m_onSession(false);
    return false;
  }
  memcpy(hmacInput, meta.data(), meta.size());
  memcpy(hmacInput + meta.size(), sessionInfo, len);

  uint8_t expected[32];
  if (!hmacSha256(m_sessionInfoKey, sizeof(m_sessionInfoKey), hmacInput,
                  meta.size() + len, expected)) {
    if (m_onSession) m_onSession(false);
    return false;
  }

  // 常量时间比较（避免时序侧信道）
  uint8_t diff = 0;
  for (size_t i = 0; i < 32; ++i) diff |= (uint8_t)(expected[i] ^ tag[i]);
  if (diff != 0) {
    CFG_LOG("[CRYPTO] 握手响应 HMAC 校验失败");
    m_hasSession = false;
    if (m_onSession) m_onSession(false);
    return false;
  }

  memcpy(m_epoch, epoch, sizeof(m_epoch));
  m_counter = counter;
  m_lastRespCounter = counter;
  m_clockBase = clockTime;
  m_clockBaseMs = millis();
  m_hasSession = true;
  m_lastFault = kFaultNone;

  CFG_LOG("[CRYPTO] 握手成功 counter=%u epoch=%02x%02x%02x%02x clock=%u K=%02x%02x%02x%02x",
          (unsigned)counter, m_epoch[0], m_epoch[1], m_epoch[2], m_epoch[3],
          (unsigned)clockTime, m_sharedKey[0], m_sharedKey[1],
          m_sharedKey[2], m_sharedKey[3]);

  if (m_onSession) m_onSession(true);
  return true;
}

bool TeslaCrypto::handleResponse(const uint8_t* ciphertext, size_t len,
                                 const uint8_t* nonce, size_t nonceLen,
                                 uint32_t counter, const uint8_t* tag,
                                 size_t tagLen, uint32_t flags, uint32_t fault) {
  m_lastFault = fault;
  if (fault != kFaultNone) {
    CFG_LOG("[CRYPTO] 命令被拒 fault=%u", (unsigned)fault);
    report(nullptr, 0, fault);
    return false;
  }
  if (!m_hasSession) {
    CFG_LOG("[CRYPTO] 收到加密响应但会话未建立");
    report(nullptr, 0, kFaultInvalidDomains);
    return false;
  }
  if (!nonce || nonceLen != 12 || !tag || tagLen != 16) {
    CFG_LOG("[CRYPTO] 响应签名字段非法 nonce=%u tag=%u", (unsigned)nonceLen,
            (unsigned)tagLen);
    report(nullptr, 0, kFaultInvalidSignature);
    return false;
  }
  if (counter <= m_lastRespCounter) {
    CFG_LOG("[CRYPTO] 响应计数器回退（疑似重放）%u <= %u", (unsigned)counter,
            (unsigned)m_lastRespCounter);
    report(nullptr, 0, kFaultInvalidSignature);
    return false;
  }
  if (len > sizeof(m_plain)) {
    CFG_LOG("[CRYPTO] 响应超过解密缓冲 %uB", (unsigned)len);
    return false;
  }

  // request_hash = 1 字节签名方式 + 请求认证 tag
  uint8_t requestHash[17];
  requestHash[0] = kSigAesGcmPersonalized;
  memcpy(requestHash + 1, m_lastReqTag, sizeof(m_lastReqTag));

  TlvBuilder meta;
  buildResponseMetadata(meta, m_vin.c_str(), counter, flags, requestHash,
                        sizeof(requestHash), fault);
  if (!meta.ok) return false;

  uint8_t aad[32];
  if (!sha256Bytes(meta.data(), meta.size(), aad)) return false;

  if (!gcmDecrypt(m_sharedKey, nonce, aad, sizeof(aad), ciphertext, len, tag,
                  m_plain)) {
    CFG_LOG("[CRYPTO] 响应 AES-GCM 解密/认证失败");
    report(nullptr, 0, kFaultInvalidSignature);
    return false;
  }

  m_lastRespCounter = counter;
  ++m_responsesOk;
  report(m_plain, len, kFaultNone);
  return true;
}

void TeslaCrypto::report(const uint8_t* plaintext, size_t len, uint32_t fault) {
  if (m_onResponse) m_onResponse(plaintext, len, fault);
}
