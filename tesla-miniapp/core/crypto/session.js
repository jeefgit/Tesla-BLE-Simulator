// Tesla 加密会话层：密钥派生 + TLV 元数据 + AES-128-GCM 封包/解包
// 对齐 simulator/tesla_protocol/{session,metadata,secure_link}.py 与 TeslaCrypto.cpp
'use strict';

const { sha1, sha256, hmacSha256, gcmEncrypt, gcmDecrypt, ecdhSharedX } = require('./index');
const { parseSessionInfo } = require('../pbCodec');
const { asciiBytes, concat, u32be } = require('../../utils/bytes');

const SESSION_INFO_LABEL = asciiBytes('session info');

// Signatures.Tag / SignatureType（metadata.py:17-34）
const TAG_SIGNATURE_TYPE = 0;
const TAG_DOMAIN = 1;
const TAG_PERSONALIZATION = 2;
const TAG_EPOCH = 3;
const TAG_EXPIRES_AT = 4;
const TAG_COUNTER = 5;
const TAG_CHALLENGE = 6;
const TAG_FLAGS = 7;
const TAG_REQUEST_HASH = 8;
const TAG_FAULT = 9;
const TAG_END = 0xff;

const SIG_TYPE_AES_GCM_PERSONALIZED = 5;
const SIG_TYPE_HMAC = 6;
const SIG_TYPE_AES_GCM_RESPONSE = 9;

const DOMAIN_INFOTAINMENT = 3;

// UniversalMessage.Flags：FLAG_ENCRYPT_RESPONSE 位号为 1（值 = 2）
const FLAG_ENCRYPT_RESPONSE = 2;

// Session_Info_Status
const SESSION_INFO_STATUS_OK = 0;
const SESSION_INFO_STATUS_KEY_NOT_ON_WHITELIST = 1;

// ── 随机数 ────────────────────────────────────────────────────────
// 小程序同步 API 无安全随机；Math.random 对模拟器联调足够，M6 换 wx.getRandomValues
function fillRandom(len) {
  const out = new Uint8Array(len);
  for (let i = 0; i < len; i++) out[i] = Math.floor(Math.random() * 256) & 0xff;
  return out;
}

// ── TLV 元数据（metadata.py）──────────────────────────────────────
// 每项 TAG || LEN(1B) || VALUE，按 tag 升序拼接，末尾 0xFF
function serializeTlv(items) {
  const sorted = items.slice().sort(function (a, b) { return a[0] - b[0]; });
  const parts = [];
  for (let i = 0; i < sorted.length; i++) {
    const tag = sorted[i][0];
    const value = sorted[i][1];
    parts.push(new Uint8Array([tag, value.length]));
    parts.push(value);
  }
  parts.push(new Uint8Array([TAG_END]));
  return concat(parts);
}

// 握手响应元数据：HMAC + VIN + challenge（metadata.session_info_metadata）
function sessionInfoMetadata(vin, challenge) {
  return serializeTlv([
    [TAG_SIGNATURE_TYPE, new Uint8Array([SIG_TYPE_HMAC])],
    [TAG_PERSONALIZATION, asciiBytes(vin)],
    [TAG_CHALLENGE, challenge],
  ]);
}

// 命令请求元数据（metadata.request_metadata；flags 仅非零写入）
function requestMetadata(vin, epoch, counter, expiresAt, flags) {
  const items = [
    [TAG_SIGNATURE_TYPE, new Uint8Array([SIG_TYPE_AES_GCM_PERSONALIZED])],
    [TAG_DOMAIN, new Uint8Array([DOMAIN_INFOTAINMENT])],
    [TAG_PERSONALIZATION, asciiBytes(vin)],
  ];
  if (epoch && epoch.length) items.push([TAG_EPOCH, epoch]);
  if (expiresAt) items.push([TAG_EXPIRES_AT, u32be(expiresAt)]);
  items.push([TAG_COUNTER, u32be(counter)]);
  if (flags) items.push([TAG_FLAGS, u32be(flags)]);
  return serializeTlv(items);
}

// 响应元数据（metadata.response_metadata；FLAGS/FAULT 总是写入）
function responseMetadata(vin, counter, flags, requestHash, fault) {
  return serializeTlv([
    [TAG_SIGNATURE_TYPE, new Uint8Array([SIG_TYPE_AES_GCM_RESPONSE])],
    [TAG_DOMAIN, new Uint8Array([DOMAIN_INFOTAINMENT])],
    [TAG_PERSONALIZATION, asciiBytes(vin)],
    [TAG_COUNTER, u32be(counter)],
    [TAG_FLAGS, u32be(flags)],
    [TAG_REQUEST_HASH, requestHash],
    [TAG_FAULT, u32be(fault)],
  ]);
}

// 响应元数据变体：部分实现省略 TAG_FAULT=0（metadata.response_metadata_candidates）
function responseMetadataCandidates(vin, counter, flags, requestHash, fault) {
  const list = [responseMetadata(vin, counter, flags, requestHash, fault)];
  if (!fault) {
    list.push(serializeTlv([
      [TAG_SIGNATURE_TYPE, new Uint8Array([SIG_TYPE_AES_GCM_RESPONSE])],
      [TAG_DOMAIN, new Uint8Array([DOMAIN_INFOTAINMENT])],
      [TAG_PERSONALIZATION, asciiBytes(vin)],
      [TAG_COUNTER, u32be(counter)],
      [TAG_FLAGS, u32be(flags)],
      [TAG_REQUEST_HASH, requestHash],
    ]));
  }
  return list;
}

// ── 常量时间比较 ──────────────────────────────────────────────────
function constantTimeEquals(a, b) {
  if (!a || !b || a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; i++) diff |= a[i] ^ b[i];
  return diff === 0;
}

// ── 会话 ──────────────────────────────────────────────────────────
class TeslaSession {
  constructor(vin) {
    this.vin = vin;
    this.reset();
  }

  reset() {
    this.hasSession = false;
    this.sharedKey = null;       // K = SHA1(Sx)[:16]
    this.sessionInfoKey = null;  // HMAC-SHA256(K, "session info")
    this.epoch = null;
    this.counter = 0;            // 客户端命令计数器（握手 counter 起，命令前 +1）
    this.clockBase = 0;          // 车端握手时刻（秒）
    this.clockBaseMs = 0;        // 本地毫秒基准
    this.lastRespCounter = 0;    // 响应防重放（每条新请求重置为 0）
  }

  // 完成握手：校验 SessionInfo + HMAC tag，派生会话密钥
  // 返回 { ok:true, counter, epoch, clockTime } 或 { ok:false, reason, notPaired }
  completeHandshake(privKey, challenge, sessionInfoBytes, tagBytes) {
    const info = parseSessionInfo(sessionInfoBytes);
    if (!info) return { ok: false, reason: 'SessionInfo 解析失败' };
    if (!info.publicKey || info.publicKey.length !== 65 || info.publicKey[0] !== 0x04) {
      return { ok: false, reason: '车辆公钥非法 (' + (info.publicKey ? info.publicKey.length : 0) + 'B)' };
    }
    if (!info.epoch || info.epoch.length !== 16) {
      return { ok: false, reason: 'epoch 非法' };
    }
    if (info.status === SESSION_INFO_STATUS_KEY_NOT_ON_WHITELIST) {
      return { ok: false, reason: 'KEY_NOT_ON_WHITELIST（本机公钥未配对）', notPaired: true };
    }
    if (info.status !== SESSION_INFO_STATUS_OK) {
      return { ok: false, reason: '握手被拒 status=' + info.status };
    }
    if (!tagBytes || tagBytes.length !== 32) {
      return { ok: false, reason: '缺少 session_info_tag' };
    }

    // ECDH → K = SHA1(Sx)[:16] → SESSION_INFO_KEY
    const sharedX = ecdhSharedX(privKey, info.publicKey);
    if (!sharedX) return { ok: false, reason: 'ECDH 失败' };
    const sharedKey = sha1(sharedX).subarray(0, 16);
    const sessionInfoKey = hmacSha256(sharedKey, SESSION_INFO_LABEL);

    // HMAC(sessionInfoKey, TLV(sessionInfoMetadata) || sessionInfoBytes)
    const meta = sessionInfoMetadata(this.vin, challenge);
    const expected = hmacSha256(sessionInfoKey, concat([meta, sessionInfoBytes]));
    if (!constantTimeEquals(expected, tagBytes)) {
      return { ok: false, reason: '握手响应 HMAC 校验失败' };
    }

    this.sharedKey = sharedKey;
    this.sessionInfoKey = sessionInfoKey;
    this.epoch = info.epoch.slice();
    this.counter = info.counter;
    // SessionInfo.counter 不是响应计数器起点（真车响应从 1 开始），lastRespCounter 固定 0
    this.lastRespCounter = 0;
    this.clockBase = info.clockTime;
    this.clockBaseMs = Date.now();
    this.hasSession = true;
    return { ok: true, counter: info.counter, clockTime: info.clockTime, keyFingerprint: sharedKey[0] };
  }

  // 车端域时钟（秒）：握手时钟 + 本地流逝
  domainNow() {
    return this.clockBase + Math.floor((Date.now() - this.clockBaseMs) / 1000);
  }

  // 加密一条命令明文 → { nonce, ciphertext, tag, counter, expiresAt }
  sealCommand(plain, flags) {
    if (!this.hasSession) return null;
    const counter = (this.counter + 1) >>> 0;
    const expiresAt = this.domainNow() + 60; // COMMAND_TTL_S
    const meta = requestMetadata(this.vin, this.epoch, counter, expiresAt, flags);
    const aad = sha256(meta);
    const nonce = fillRandom(12);
    const sealed = gcmEncrypt(this.sharedKey, nonce, aad, plain);
    this.counter = counter; // 发送成功才算（调用方失败可自行回退）
    // 防重放窗口按"每次请求一个"重置（真车每条响应计数器从 1 开始；
    // 模拟器为会话单调递增，同样满足 counter > 0）
    this.lastRespCounter = 0;
    return {
      nonce: nonce,
      ciphertext: sealed.ciphertext,
      tag: sealed.tag,
      counter: counter,
      expiresAt: expiresAt,
    };
  }

  // 解密一条加密响应 → Uint8Array 明文 或 null
  // reqTag 为本条请求的 GCM tag（构造 request_hash）
  openResponse(ciphertext, nonce, counter, tag, fault, flags, reqTag) {
    if (!this.hasSession) return null;
    if (counter <= this.lastRespCounter) return null; // 疑似重放
    // request_hash = 1 字节签名方式 + 请求认证 tag（secure_link.py:114-119）
    const requestHash = concat([new Uint8Array([SIG_TYPE_AES_GCM_PERSONALIZED]), reqTag]);
    const metas = responseMetadataCandidates(this.vin, counter, flags, requestHash, fault);
    for (let i = 0; i < metas.length; i++) {
      const aad = sha256(metas[i]);
      const plain = gcmDecrypt(this.sharedKey, nonce, aad, ciphertext, tag);
      if (plain) {
        this.lastRespCounter = counter;
        return plain;
      }
    }
    return null;
  }
}

module.exports = {
  TeslaSession,
  sessionInfoMetadata,
  requestMetadata,
  responseMetadata,
  responseMetadataCandidates,
  serializeTlv,
  fillRandom,
  FLAG_ENCRYPT_RESPONSE,
  DOMAIN_INFOTAINMENT,
  SESSION_INFO_STATUS_KEY_NOT_ON_WHITELIST,
  SIG_TYPE_AES_GCM_PERSONALIZED,
};
