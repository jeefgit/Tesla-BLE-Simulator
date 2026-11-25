// 极简 protobuf 编解码：只覆盖 Tesla BLE 协议需要的 wire 类型与消息
// 字段号权威来源：simulator/tesla_protocol/protobuf/*.proto 与
// ESP32 TeslaCrypto.cpp（buildHandshakeRequest / buildGetVehicleData / buildPairingRequest）
'use strict';

const WIRE_VARINT = 0;
const WIRE_FIXED64 = 1;
const WIRE_LEN = 2;
const WIRE_FIXED32 = 5;

// ── Writer ────────────────────────────────────────────────────────
class PbWriter {
  constructor() {
    this._buf = new Uint8Array(64);
    this._len = 0;
  }

  _ensure(n) {
    if (this._len + n <= this._buf.length) return;
    let cap = this._buf.length;
    while (cap < this._len + n) cap *= 2;
    const b = new Uint8Array(cap);
    b.set(this._buf.subarray(0, this._len));
    this._buf = b;
  }

  _byte(x) {
    this._ensure(1);
    this._buf[this._len++] = x & 0xff;
  }

  // number（< 2^53）→ varint 字节序列
  _varintBytes(v) {
    const out = [];
    let x = v;
    do {
      let b = x % 128;
      x = Math.floor(x / 128);
      if (x > 0) b |= 0x80;
      out.push(b);
    } while (x > 0);
    return out;
  }

  _tag(field, wire) {
    const t = this._varintBytes((field << 3) | wire);
    for (let i = 0; i < t.length; i++) this._byte(t[i]);
  }

  putVarint(field, v) {
    this._tag(field, WIRE_VARINT);
    const t = this._varintBytes(v >>> 0);
    for (let i = 0; i < t.length; i++) this._byte(t[i]);
    return this;
  }

  putBytes(field, bytes) {
    this._tag(field, WIRE_LEN);
    const t = this._varintBytes(bytes.length);
    for (let i = 0; i < t.length; i++) this._byte(t[i]);
    this._ensure(bytes.length);
    this._buf.set(bytes, this._len);
    this._len += bytes.length;
    return this;
  }

  // 嵌套消息：接受 PbWriter 或 Uint8Array
  putMessage(field, msg) {
    const bytes = msg instanceof Uint8Array ? msg : msg.finish();
    return this.putBytes(field, bytes);
  }

  // 空消息（GetVehicleData 的 getState 子消息等）
  putEmptyMessage(field) {
    this._tag(field, WIRE_LEN);
    this._byte(0);
    return this;
  }

  putFixed32(field, v) {
    this._tag(field, WIRE_FIXED32);
    this._ensure(4);
    this._buf[this._len] = v & 0xff;
    this._buf[this._len + 1] = (v >>> 8) & 0xff;
    this._buf[this._len + 2] = (v >>> 16) & 0xff;
    this._buf[this._len + 3] = (v >>> 24) & 0xff;
    this._len += 4;
    return this;
  }

  // proto float 字段 = fixed32 wire type + IEEE754 LE
  putFloat32(field, v) {
    this._tag(field, WIRE_FIXED32);
    this._ensure(4);
    const dv = new DataView(this._buf.buffer, this._buf.byteOffset + this._len, 4);
    dv.setFloat32(0, v, true);
    this._len += 4;
    return this;
  }

  finish() {
    return this._buf.subarray(0, this._len).slice();
  }
}

// ── Reader ────────────────────────────────────────────────────────
class PbReader {
  constructor(bytes) {
    this._b = bytes || new Uint8Array(0);
    this._o = 0;
  }

  atEnd() {
    return this._o >= this._b.length;
  }

  // 读一个 varint（< 2^53）；越界/过长返回 null
  _varint() {
    let v = 0;
    let shift = 0;
    for (;;) {
      if (this._o >= this._b.length || shift > 49) return null;
      const b = this._b[this._o++];
      v += (b & 0x7f) * Math.pow(2, shift);
      if (!(b & 0x80)) return v;
      shift += 7;
    }
  }

  // 返回 { field, wire }；解析失败返回 null
  next() {
    const save = this._o;
    const tag = this._varint();
    if (tag === null) { this._o = save; return null; }
    const field = tag >>> 3;
    const wire = tag & 7;
    if (field === 0 || wire === WIRE_FIXED64) return null; // FIXED64 不支持，视为损坏
    return { field, wire };
  }

  readVarint() {
    const save = this._o;
    const v = this._varint();
    if (v === null) this._o = save;
    return v;
  }

  readBytes() {
    const save = this._o;
    const len = this._varint();
    if (len === null || this._o + len > this._b.length) { this._o = save; return null; }
    const out = this._b.subarray(this._o, this._o + len);
    this._o += len;
    return out;
  }

  readFixed32() {
    if (this._o + 4 > this._b.length) return null;
    const dv = new DataView(this._b.buffer, this._b.byteOffset + this._o, 4);
    this._o += 4;
    return dv.getUint32(0, true);
  }

  readFloat32() {
    if (this._o + 4 > this._b.length) return null;
    const dv = new DataView(this._b.buffer, this._b.byteOffset + this._o, 4);
    this._o += 4;
    return dv.getFloat32(0, true);
  }

  // varint 有符号 int32（proto3 负数按二补码 64bit varint 编码）
  readInt32() {
    const v = this.readVarint();
    if (v === null) return null;
    return v >= 0x80000000 ? v - 0x100000000 : v;
  }

  skip(wire) {
    if (wire === WIRE_VARINT) return this.readVarint() !== null;
    if (wire === WIRE_FIXED32) return this._o + 4 <= this._b.length && ((this._o += 4), true);
    if (wire === WIRE_LEN) return this.readBytes() !== null;
    return false; // FIXED64 及组已由 next() 拦截
  }
}

// ── 帧封装：2 字节大端长度前缀（secure_link.py:20-33）──────────────
function frameOf(body) {
  const out = new Uint8Array(body.length + 2);
  out[0] = (body.length >> 8) & 0xff;
  out[1] = body.length & 0xff;
  out.set(body, 2);
  return out;
}

// ── UniversalMessage.RoutableMessage 字段号 ───────────────────────
const RM = {
  TO_DEST: 6, FROM_DEST: 7, PAYLOAD: 10, SIG_DATA: 13, MSG_STATUS: 12,
  SESSION_INFO_REQ: 14, SESSION_INFO: 15, REQUEST_UUID: 50, UUID: 51, FLAGS: 52,
};

// 解析入站 RoutableMessage（允许部分字段缺失）
function parseRoutable(bytes) {
  const out = {
    toDomain: null, fromDomain: null,
    ciphertext: null, sessionInfo: null,
    fault: 0, opStatus: null, flags: 0,
    sessionInfoTag: null, gcm: null, resp: null,
    requestUuid: null,
  };
  const r = new PbReader(bytes);
  while (!r.atEnd()) {
    const h = r.next();
    if (!h) return null;
    if (h.field === RM.TO_DEST && h.wire === WIRE_LEN) {
      const d = r.readBytes();
      if (!d) return null;
      out.toDomain = parseDestination(d);
    } else if (h.field === RM.FROM_DEST && h.wire === WIRE_LEN) {
      const d = r.readBytes();
      if (!d) return null;
      out.fromDomain = parseDestination(d);
    } else if (h.field === RM.PAYLOAD && h.wire === WIRE_LEN) {
      out.ciphertext = r.readBytes();
      if (!out.ciphertext) return null;
    } else if (h.field === RM.MSG_STATUS && h.wire === WIRE_LEN) {
      const d = r.readBytes();
      if (!d) return null;
      const st = parseMessageStatus(d);
      if (!st) return null;
      out.fault = st.fault;
      out.opStatus = st.opStatus;
    } else if (h.field === RM.SESSION_INFO && h.wire === WIRE_LEN) {
      out.sessionInfo = r.readBytes();
      if (!out.sessionInfo) return null;
    } else if (h.field === RM.SIG_DATA && h.wire === WIRE_LEN) {
      const d = r.readBytes();
      if (!d) return null;
      const sig = parseSignatureData(d);
      if (!sig) return null;
      out.sessionInfoTag = sig.sessionInfoTag;
      out.gcm = sig.gcm;
      out.resp = sig.resp;
    } else if (h.field === RM.FLAGS && h.wire === WIRE_VARINT) {
      const v = r.readVarint();
      if (v === null) return null;
      out.flags = v >>> 0;
    } else if (h.field === RM.REQUEST_UUID && h.wire === WIRE_LEN) {
      out.requestUuid = r.readBytes();
      if (!out.requestUuid) return null;
    } else if (!r.skip(h.wire)) {
      return null;
    }
  }
  return out;
}

// Destination.domain（field 1 varint）
function parseDestination(bytes) {
  const r = new PbReader(bytes);
  let domain = null;
  while (!r.atEnd()) {
    const h = r.next();
    if (!h) return null;
    if (h.field === 1 && h.wire === WIRE_VARINT) {
      domain = r.readVarint();
      if (domain === null) return null;
    } else if (!r.skip(h.wire)) {
      return null;
    }
  }
  return domain;
}

function parseMessageStatus(bytes) {
  const r = new PbReader(bytes);
  const out = { opStatus: null, fault: 0 };
  while (!r.atEnd()) {
    const h = r.next();
    if (!h) return null;
    if (h.field === 1 && h.wire === WIRE_VARINT) {
      out.opStatus = r.readVarint();
      if (out.opStatus === null) return null;
    } else if (h.field === 2 && h.wire === WIRE_VARINT) {
      const v = r.readVarint();
      if (v === null) return null;
      out.fault = v >>> 0;
    } else if (!r.skip(h.wire)) {
      return null;
    }
  }
  return out;
}

// Signatures.SignatureData：session_info_tag(6) / AES_GCM_Personalized_data(5) / Response_data(9)
function parseSignatureData(bytes) {
  const out = { sessionInfoTag: null, gcm: null, resp: null };
  const r = new PbReader(bytes);
  while (!r.atEnd()) {
    const h = r.next();
    if (!h) return null;
    if (h.field === 6 && h.wire === WIRE_LEN) {
      const d = r.readBytes();
      if (!d) return null;
      out.sessionInfoTag = parseHmacTag(d);
      if (!out.sessionInfoTag) return null;
    } else if (h.field === 5 && h.wire === WIRE_LEN) {
      const d = r.readBytes();
      if (!d) return null;
      out.gcm = parseGcmData(d);
      if (!out.gcm) return null;
    } else if (h.field === 9 && h.wire === WIRE_LEN) {
      const d = r.readBytes();
      if (!d) return null;
      out.resp = parseRespData(d);
      if (!out.resp) return null;
    } else if (!r.skip(h.wire)) {
      return null;
    }
  }
  return out;
}

// HMAC_Signature_Data{ tag = 1 }
function parseHmacTag(bytes) {
  const r = new PbReader(bytes);
  let tag = null;
  while (!r.atEnd()) {
    const h = r.next();
    if (!h) return null;
    if (h.field === 1 && h.wire === WIRE_LEN) {
      tag = r.readBytes();
      if (!tag) return null;
    } else if (!r.skip(h.wire)) {
      return null;
    }
  }
  return tag;
}

// AES_GCM_Personalized_Signature_Data{ epoch=1, nonce=2, counter=3, expires_at=4, tag=5 }
function parseGcmData(bytes) {
  const out = { epoch: null, nonce: null, counter: 0, expiresAt: 0, tag: null };
  const r = new PbReader(bytes);
  while (!r.atEnd()) {
    const h = r.next();
    if (!h) return null;
    if (h.field === 1 && h.wire === WIRE_LEN) {
      out.epoch = r.readBytes();
      if (!out.epoch) return null;
    } else if (h.field === 2 && h.wire === WIRE_LEN) {
      out.nonce = r.readBytes();
      if (!out.nonce) return null;
    } else if (h.field === 3 && h.wire === WIRE_VARINT) {
      const v = r.readVarint();
      if (v === null) return null;
      out.counter = v >>> 0;
    } else if (h.field === 4 && h.wire === WIRE_FIXED32) {
      const v = r.readFixed32();
      if (v === null) return null;
      out.expiresAt = v;
    } else if (h.field === 5 && h.wire === WIRE_LEN) {
      out.tag = r.readBytes();
      if (!out.tag) return null;
    } else if (!r.skip(h.wire)) {
      return null;
    }
  }
  return out;
}

// AES_GCM_Response_Signature_Data{ nonce=1, counter=2, tag=3 }
function parseRespData(bytes) {
  const out = { nonce: null, counter: 0, tag: null };
  const r = new PbReader(bytes);
  while (!r.atEnd()) {
    const h = r.next();
    if (!h) return null;
    if (h.field === 1 && h.wire === WIRE_LEN) {
      out.nonce = r.readBytes();
      if (!out.nonce) return null;
    } else if (h.field === 2 && h.wire === WIRE_VARINT) {
      const v = r.readVarint();
      if (v === null) return null;
      out.counter = v >>> 0;
    } else if (h.field === 3 && h.wire === WIRE_LEN) {
      out.tag = r.readBytes();
      if (!out.tag) return null;
    } else if (!r.skip(h.wire)) {
      return null;
    }
  }
  return out;
}

// ── Signatures.SessionInfo ────────────────────────────────────────
function parseSessionInfo(bytes) {
  const out = { counter: 0, publicKey: null, epoch: null, clockTime: 0, status: 0 };
  const r = new PbReader(bytes);
  while (!r.atEnd()) {
    const h = r.next();
    if (!h) return null;
    if (h.field === 1 && h.wire === WIRE_VARINT) {
      const v = r.readVarint();
      if (v === null) return null;
      out.counter = v >>> 0;
    } else if (h.field === 2 && h.wire === WIRE_LEN) {
      out.publicKey = r.readBytes();
      if (!out.publicKey) return null;
    } else if (h.field === 3 && h.wire === WIRE_LEN) {
      out.epoch = r.readBytes();
      if (!out.epoch) return null;
    } else if (h.field === 4 && h.wire === WIRE_FIXED32) {
      const v = r.readFixed32();
      if (v === null) return null;
      out.clockTime = v;
    } else if (h.field === 5 && h.wire === WIRE_VARINT) {
      const v = r.readVarint();
      if (v === null) return null;
      out.status = v >>> 0;
    } else if (!r.skip(h.wire)) {
      return null;
    }
  }
  return out;
}

// ── 握手请求（TeslaCrypto.cpp buildHandshakeRequest）──────────────
function buildHandshakeRequest(pub, routing, challenge) {
  const dest = new PbWriter();
  dest.putVarint(1, 3); // DOMAIN_INFOTAINMENT

  const from = new PbWriter();
  from.putBytes(2, routing);

  const sir = new PbWriter();
  sir.putBytes(1, pub); // SessionInfoRequest.public_key

  const w = new PbWriter();
  w.putMessage(6, dest);   // to_destination
  w.putMessage(7, from);   // from_destination
  w.putMessage(14, sir);   // session_info_request
  w.putBytes(51, challenge); // uuid
  return w.finish();
}

// ── GetVehicleData 加密命令（TeslaCrypto.cpp buildGetVehicleData）──
// req: { kind, epoch, nonce, counter, expiresAt, tag, routing, pub, flags, uuid }
// kind: 'drive' | 'charge' | 'climate' | 'closures' | 'tire'
// plain: 明文 CarServer.Action（供会话层加密）；body: 完整 RoutableMessage
function buildGetVehicleDataAction(kind) {
  const gvd = new PbWriter();
  const map = { drive: 4, charge: 2, climate: 3, closures: 8, tire: 14 };
  gvd.putEmptyMessage(map[kind]);

  const va = new PbWriter();
  va.putMessage(1, gvd); // VehicleAction.getVehicleData

  const action = new PbWriter();
  action.putMessage(2, va); // Action.vehicleAction
  return action.finish();
}

function buildGetVehicleData(req) {
  const dest = new PbWriter();
  dest.putVarint(1, 3);

  const from = new PbWriter();
  from.putBytes(2, req.routing);

  const kid = new PbWriter();
  kid.putBytes(1, req.pub); // signer_identity.public_key

  const gcm = new PbWriter();
  gcm.putBytes(1, req.epoch);
  gcm.putBytes(2, req.nonce);
  gcm.putVarint(3, req.counter);
  gcm.putFixed32(4, req.expiresAt);
  gcm.putBytes(5, req.tag);

  const sig = new PbWriter();
  sig.putMessage(1, kid);
  sig.putMessage(5, gcm);

  const w = new PbWriter();
  w.putMessage(6, dest);
  w.putMessage(7, from);
  w.putBytes(10, req.ciphertext); // protobuf_message_as_bytes
  w.putMessage(13, sig);
  w.putVarint(52, req.flags);
  w.putBytes(51, req.uuid);
  return w.finish();
}

// ── add-key-request 明文配钥（TeslaCrypto.cpp buildPairingRequest）─
function buildPairingRequest(pub, role, formFactor) {
  const key = new PbWriter();
  key.putBytes(1, pub); // PublicKey.PublicKeyRaw

  const change = new PbWriter();
  change.putMessage(1, key);
  change.putVarint(4, role); // keyRole

  const meta = new PbWriter();
  meta.putVarint(1, formFactor); // keyFormFactor

  const op = new PbWriter();
  op.putMessage(5, change); // addKeyToWhitelistAndAddPermissions
  op.putMessage(6, meta);   // metadataForKey

  const um = new PbWriter();
  um.putMessage(16, op); // UnsignedMessage.WhitelistOperation

  const sm = new PbWriter();
  sm.putBytes(2, um.finish()); // SignedMessage.protobufMessageAsBytes
  sm.putVarint(3, 2);          // SIGNATURE_TYPE_PRESENT_KEY

  const w = new PbWriter();
  w.putMessage(1, sm); // ToVCSECMessage.signedMessage
  return w.finish();
}

// ── 配钥状态解析（TeslaCrypto.cpp parsePairingStatus，A/B/C 三种封装）──
// CommandStatus: operationStatus=1, whitelistOperationStatus=3
// WhitelistOperation_status: whitelistOperationInformation=1, operationStatus=3
function parseCommandStatus(bytes) {
  const out = { opStatus: 0, info: 0, terminal: false, found: false };
  const r = new PbReader(bytes);
  while (!r.atEnd()) {
    const h = r.next();
    if (!h) return null;
    if (h.field === 1 && h.wire === WIRE_VARINT) {
      const v = r.readVarint();
      if (v === null) return null;
      out.opStatus = v >>> 0;
      out.found = true;
    } else if (h.field === 3 && h.wire === WIRE_LEN) {
      const wl = r.readBytes();
      if (!wl) return null;
      out.terminal = true;
      out.found = true;
      const r2 = new PbReader(wl);
      while (!r2.atEnd()) {
        const h2 = r2.next();
        if (!h2) return null;
        if (h2.field === 1 && h2.wire === WIRE_VARINT) {
          const v = r2.readVarint();
          if (v === null) return null;
          out.info = v >>> 0;
        } else if (!r2.skip(h2.wire)) {
          return null;
        }
      }
    } else if (!r.skip(h.wire)) {
      return null;
    }
  }
  return out;
}

function parsePairingStatus(bytes) {
  // A/C：FromVCSECMessage{ commandStatus = 4 }
  let r = new PbReader(bytes);
  while (!r.atEnd()) {
    const h = r.next();
    if (!h) break;
    if (h.field === 4 && h.wire === WIRE_LEN) {
      const cs = r.readBytes();
      if (!cs) break;
      const st = parseCommandStatus(cs);
      if (st && st.found) return st;
    } else if (!r.skip(h.wire)) {
      break;
    }
  }
  // B/C：裸 CommandStatus
  return parseCommandStatus(bytes);
}

module.exports = {
  WIRE_VARINT,
  WIRE_LEN,
  WIRE_FIXED32,
  PbWriter,
  PbReader,
  frameOf,
  parseRoutable,
  parseSessionInfo,
  buildHandshakeRequest,
  buildGetVehicleDataAction,
  buildGetVehicleData,
  buildPairingRequest,
  parsePairingStatus,
  RM,
};
