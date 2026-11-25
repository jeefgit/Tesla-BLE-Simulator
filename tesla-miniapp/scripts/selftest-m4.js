// M4 自测：pbCodec / TLV / 会话握手 / 命令加解密 / CarServer.Response 解码
// 对拍基准：node 官方 crypto 独立实现"车端"（ECDH/AES-128-GCM/HMAC-SHA256/SHA1）
// 运行：node scripts/selftest-m4.js
'use strict';

const nodeCrypto = require('crypto');
const path = require('path');

const ROOT = path.join(__dirname, '..');
const pb = require(path.join(ROOT, 'core/pbCodec'));
const { parseCarServerResponse } = require(path.join(ROOT, 'core/carServer'));
const sessionCrypto = require(path.join(ROOT, 'core/crypto/session'));
const { TeslaSession, requestMetadata } = sessionCrypto;
const { sha1, hmacSha256, gcmEncrypt, gcmDecrypt, derivePublicKey, ecdhSharedX } =
  require(path.join(ROOT, 'core/crypto/index'));
const { fromHex, toHex, concat, u32be } = require(path.join(ROOT, 'utils/bytes'));

let passed = 0;
let failed = 0;
function ok(cond, name) {
  if (cond) { passed++; }
  else { failed++; console.log('  FAIL: ' + name); }
}
function eq(a, b, name) {
  const sa = a instanceof Uint8Array ? toHex(a) : String(a);
  const sb = b instanceof Uint8Array ? toHex(b) : String(b);
  ok(sa === sb, name + ' (got ' + sa + ' want ' + sb + ')');
}

// 测试客户端私钥（与 config/secret.example.js 模板一致）
const CLIENT_PRIV_HEX = 'E861650948F023C7E541A2BE5C0595A6D15AD130A72C86ADD74B73774F619FA8';
const CLIENT_PRIV = fromHex(CLIENT_PRIV_HEX);
const CLIENT_PUB = derivePublicKey(CLIENT_PRIV);
const VIN = 'LRWYGCFJ8RC571213';

// ── 独立"车端"实现（node crypto，供对拍）────────────────────────
function nodeSha1(b) { return new Uint8Array(nodeCrypto.createHash('sha1').update(Buffer.from(b)).digest()); }
function nodeSha256(b) { return new Uint8Array(nodeCrypto.createHash('sha256').update(Buffer.from(b)).digest()); }
function nodeHmac(key, data) {
  return new Uint8Array(nodeCrypto.createHmac('sha256', Buffer.from(key)).update(Buffer.from(data)).digest());
}
function nodeGcmEncrypt(key, nonce, aad, plain) {
  const c = nodeCrypto.createCipheriv('aes-128-gcm', Buffer.from(key), Buffer.from(nonce));
  c.setAAD(Buffer.from(aad));
  const ct = Buffer.concat([c.update(Buffer.from(plain)), c.final()]);
  return { ciphertext: new Uint8Array(ct), tag: new Uint8Array(c.getAuthTag()) };
}
function nodeGcmDecrypt(key, nonce, aad, ct, tag) {
  try {
    const d = nodeCrypto.createDecipheriv('aes-128-gcm', Buffer.from(key), Buffer.from(nonce));
    d.setAAD(Buffer.from(aad));
    d.setAuthTag(Buffer.from(tag));
    const pt = Buffer.concat([d.update(Buffer.from(ct)), d.final()]);
    return new Uint8Array(pt);
  } catch (e) { return null; }
}

// 车端 TLV 独立构造（Buffer 手工拼，验证我们的 serializeTlv 与规范一致）
// 注意：sessionInfoMetadata 只有 SIG_TYPE + PERSONALIZATION + CHALLENGE，无 DOMAIN
// （metadata.py:62-70 与 TeslaCrypto.cpp buildSessionInfoMetadata 一致）
function serverSessionInfoMeta(vin, challenge) {
  return Buffer.concat([
    Buffer.from([0, 1, 6]),                       // TAG_SIGNATURE_TYPE = HMAC(6)
    Buffer.from([2, vin.length]), Buffer.from(vin, 'utf8'),
    Buffer.from([6, challenge.length]), Buffer.from(challenge),
    Buffer.from([0xff]),
  ]);
}
function serverRequestMeta(vin, epoch, counter, expiresAt, flags) {
  return Buffer.concat([
    Buffer.from([0, 1, 5]),                       // SIG_TYPE = AES_GCM_PERSONALIZED(5)
    Buffer.from([1, 1, 3]),
    Buffer.from([2, vin.length]), Buffer.from(vin, 'utf8'),
    Buffer.from([3, 16]), Buffer.from(epoch),
    Buffer.from([4, 4]), u32be(expiresAt),        // 大端 u32
    Buffer.from([5, 4]), u32be(counter),
    Buffer.from([7, 4]), u32be(flags),
    Buffer.from([0xff]),
  ]);
}
function serverResponseMeta(vin, counter, flags, requestHash, fault) {
  return Buffer.concat([
    Buffer.from([0, 1, 9]),                       // SIG_TYPE = AES_GCM_RESPONSE(9)
    Buffer.from([1, 1, 3]),
    Buffer.from([2, vin.length]), Buffer.from(vin, 'utf8'),
    Buffer.from([5, 4]), u32be(counter),
    Buffer.from([7, 4]), u32be(flags),
    Buffer.from([8, requestHash.length]), Buffer.from(requestHash),
    Buffer.from([9, 4]), u32be(fault),
    Buffer.from([0xff]),
  ]);
}

// ── 1. pbCodec 基础 ───────────────────────────────────────────────
console.log('1. pbCodec 基础');
{
  // varint 往返（含大值与负数 int32）
  const w = new pb.PbWriter();
  w.putVarint(1, 0).putVarint(2, 127).putVarint(3, 128).putVarint(4, 300)
   .putVarint(5, 4294967295);
  const r = new pb.PbReader(w.finish());
  const vals = [];
  while (!r.atEnd()) {
    const h = r.next();
    vals.push([h.field, r.readVarint()]);
  }
  eq(JSON.stringify(vals.map((v) => v[1])), JSON.stringify([0, 127, 128, 300, 4294967295]), 'varint 往返');

  // int32 负数（二补码）
  const w2 = new pb.PbWriter();
  w2.putVarint(1, (-10) >>> 0);
  const r2 = new pb.PbReader(w2.finish());
  r2.next();
  eq(r2.readInt32(), -10, 'int32 负数解码');

  // fixed32
  const w3 = new pb.PbWriter();
  w3.putFixed32(1, 0x12345678);
  const r3 = new pb.PbReader(w3.finish());
  r3.next();
  eq(r3.readFixed32(), 0x12345678, 'fixed32');

  // float32（fixed32 wire type + IEEE754 LE）
  const w4 = new pb.PbWriter();
  w4.putFloat32(1, 123.5);
  const r4 = new pb.PbReader(w4.finish());
  r4.next();
  const fv = r4.readFloat32();
  ok(fv !== null && Math.abs(fv - 123.5) < 1e-5, 'float32 往返');
  ok(pb.WIRE_LEN === 2, 'WIRE_LEN=2');
}

// ── 2. 握手请求构造 ───────────────────────────────────────────────
console.log('2. 握手请求构造');
{
  const routing = new Uint8Array([0xde, 0xad, 0xbe, 0xef]);
  const challenge = fromHex('00112233445566778899aabbccddeeff');
  const body = pb.buildHandshakeRequest(CLIENT_PUB, routing, challenge);
  const rm = pb.parseRoutable(body);
  ok(rm !== null, '握手请求可解析');
  eq(rm.toDomain, 3, 'to_destination.domain = Infotainment(3)');
  eq(rm.fromDomain, null, 'from_destination 缺省');
  ok(rm.sessionInfo === null && rm.sessionInfoTag === null, '无 session_info');
  // session_info_request.public_key 必须等于客户端公钥
  const r = new pb.PbReader(body);
  let pubOk = false;
  let challengeOk = false;
  while (!r.atEnd()) {
    const h = r.next();
    if (h.field === 14 && h.wire === 2) {
      const d = r.readBytes();
      const sr = new pb.PbReader(d);
      const h2 = sr.next();
      pubOk = h2.field === 1 && (function () {
        const pk = sr.readBytes();
        return toHex(pk) === toHex(CLIENT_PUB);
      })();
    } else if (h.field === 51 && h.wire === 2) {
      challengeOk = toHex(r.readBytes()) === toHex(challenge);
    } else { r.skip(h.wire); }
  }
  ok(pubOk, 'SessionInfoRequest.public_key = 客户端公钥');
  ok(challengeOk, 'RoutableMessage.uuid = challenge');
  // 帧前缀
  const framed = pb.frameOf(body);
  eq(framed[0], (body.length >> 8) & 0xff, '帧前缀高字节');
  eq(framed[1], body.length & 0xff, '帧前缀低字节');
  eq(framed.length, body.length + 2, '帧总长');
}

// ── 3. TLV 序列化与车端独立实现对拍 ───────────────────────────────
console.log('3. TLV 元数据');
{
  const epoch = fromHex('000102030405060708090a0b0c0d0e0f');
  const ours = requestMetadata(VIN, epoch, 7, 1000, 2);
  const ref = new Uint8Array(serverRequestMeta(VIN, epoch, 7, 1000, 2));
  eq(ours, ref, 'requestMetadata TLV 与车端独立构造一致');

  const challenge = fromHex('aabbccddeeff0011aabbccddeeff0011');
  const meta2 = sessionCrypto.sessionInfoMetadata(VIN, challenge);
  eq(meta2, new Uint8Array(serverSessionInfoMeta(VIN, challenge)), 'sessionInfoMetadata 一致');
}

// ── 4. E2E：握手 → 加密命令 → 加密响应（node crypto 车端对拍）────
console.log('4. E2E 会话对拍');
let sessionSharedKey;
{
  // 车端生成密钥对，用我们的公钥做 ECDH
  const serverEcdh = nodeCrypto.createECDH('prime256v1');
  serverEcdh.generateKeys();
  const serverPub = new Uint8Array(serverEcdh.getPublicKey());

  // 交叉验证我们的 ECDH 与 node 一致
  const ourShared = ecdhSharedX(CLIENT_PRIV, serverPub);
  const nodeShared = new Uint8Array(serverEcdh.computeSecret(Buffer.from(CLIENT_PUB)));
  eq(ourShared, nodeShared, 'ECDH shared_x 与 node crypto 一致');

  // 车端侧密钥派生（node crypto）
  const K = nodeSha1(nodeShared).slice(0, 16);
  sessionSharedKey = K;
  const sessionInfoKey = nodeHmac(K, Buffer.from('session info', 'utf8'));

  // 车端构造 SessionInfo + HMAC tag
  const counter = 987654;
  const epoch = fromHex('f0e0d0c0b0a090807060504030201000');
  const clockTime = 1700000000;
  const challenge = fromHex('00112233445566778899aabbccddeeff');

  const infoW = new pb.PbWriter();
  infoW.putVarint(1, counter);
  infoW.putBytes(2, serverPub);
  infoW.putBytes(3, epoch);
  infoW.putFixed32(4, clockTime);
  infoW.putVarint(5, 0);
  const sessionInfoBytes = infoW.finish();

  const meta = serverSessionInfoMeta(VIN, challenge);
  const tag = nodeHmac(sessionInfoKey, Buffer.concat([Buffer.from(meta), Buffer.from(sessionInfoBytes)]));

  const replyW = new pb.PbWriter();
  replyW.putVarint(52, 0); // flags 占位（不含也合法）
  replyW.putBytes(15, sessionInfoBytes);
  const tagW = new pb.PbWriter();
  tagW.putBytes(1, new Uint8Array(tag));
  const sigW = new pb.PbWriter();
  sigW.putMessage(6, tagW); // session_info_tag
  replyW.putMessage(13, sigW);
  const replyBody = replyW.finish();

  // 客户端完成握手
  const sess = new TeslaSession(VIN);
  const res = sess.completeHandshake(CLIENT_PRIV, challenge, sessionInfoBytes, new Uint8Array(tag));
  ok(res.ok === true, '握手成功（' + (res.reason || 'ok') + '）');
  eq(res.counter, counter, '握手 counter');

  // 错误 challenge 应校验失败
  const bad = new TeslaSession(VIN);
  const res2 = bad.completeHandshake(CLIENT_PRIV, fromHex('ffffeeddccbbaa998877665544332211'), sessionInfoBytes, new Uint8Array(tag));
  ok(res2.ok === false, 'challenge 不匹配 → HMAC 校验失败');

  // status=1 → 未配对
  const infoW2 = new pb.PbWriter();
  infoW2.putVarint(1, counter);
  infoW2.putBytes(2, serverPub);
  infoW2.putBytes(3, epoch);
  infoW2.putFixed32(4, clockTime);
  infoW2.putVarint(5, 1); // KEY_NOT_ON_WHITELIST
  const badSess = new TeslaSession(VIN);
  const res3 = badSess.completeHandshake(CLIENT_PRIV, challenge, infoW2.finish(), null);
  ok(res3.ok === false && res3.notPaired === true, 'status=KEY_NOT_ON_WHITELIST → notPaired');

  // 客户端加密一条 Drive 轮询，车端解密
  const plain = pb.buildGetVehicleDataAction('drive');
  const sealed = sess.sealCommand(plain, sessionCrypto.FLAG_ENCRYPT_RESPONSE);
  ok(sealed !== null && sealed.counter === counter + 1, '命令计数器 = 握手 counter + 1');

  const reqMeta = serverRequestMeta(VIN, epoch, sealed.counter, sealed.expiresAt, 2);
  const roundPlain = nodeGcmDecrypt(K, sealed.nonce, nodeSha256(reqMeta), sealed.ciphertext, sealed.tag);
  ok(roundPlain !== null && toHex(roundPlain) === toHex(plain), '车端 AES-GCM 解密命令明文一致');

  // 车端构造加密响应：CarServer.Response
  const respPlain = buildTestCarServerResponse();
  const respCounter = counter + 1; // 首条响应
  const requestHash = Buffer.concat([Buffer.from([5]), Buffer.from(sealed.tag)]);
  const respMeta = serverResponseMeta(VIN, respCounter, 2, requestHash, 0);
  const respNonce = new Uint8Array(nodeCrypto.randomBytes(12));
  const enc = nodeGcmEncrypt(K, respNonce, nodeSha256(respMeta), respPlain);

  const respRm = new pb.PbWriter();
  respRm.putBytes(10, enc.ciphertext);
  const rd = new pb.PbWriter();
  rd.putBytes(1, respNonce);
  rd.putVarint(2, respCounter);
  rd.putBytes(3, enc.tag);
  const rsig = new pb.PbWriter();
  rsig.putMessage(9, rd);
  respRm.putMessage(13, rsig);
  respRm.putVarint(52, 2); // flags

  const rm = pb.parseRoutable(respRm.finish());
  ok(rm !== null && rm.resp !== null, '加密响应 RoutableMessage 可解析');
  const opened = sess.openResponse(rm.ciphertext, rm.resp.nonce, rm.resp.counter,
    rm.resp.tag, 0, rm.flags, sealed.tag);
  ok(opened !== null && toHex(opened) === toHex(respPlain), '客户端解密响应明文一致');

  // 重放同 counter 应被拒
  const replay = sess.openResponse(rm.ciphertext, rm.resp.nonce, rm.resp.counter,
    rm.resp.tag, 0, rm.flags, sealed.tag);
  eq(replay, null, '重放响应被拒');

  // 篡改密文应认证失败
  const tampered = rm.ciphertext.slice();
  tampered[0] ^= 0xff;
  const bad2 = new TeslaSession(VIN);
  bad2.completeHandshake(CLIENT_PRIV, challenge, sessionInfoBytes, new Uint8Array(tag));
  const badOpen = bad2.openResponse(tampered, rm.resp.nonce, rm.resp.counter,
    rm.resp.tag, 0, rm.flags, sealed.tag);
  eq(badOpen, null, '篡改密文 → 认证失败');

  // 错误 request_hash 应失败
  const bad3 = new TeslaSession(VIN);
  bad3.completeHandshake(CLIENT_PRIV, challenge, sessionInfoBytes, new Uint8Array(tag));
  const badOpen2 = bad3.openResponse(rm.ciphertext, rm.resp.nonce, rm.resp.counter,
    rm.resp.tag, 0, rm.flags, new Uint8Array(16));
  eq(badOpen2, null, '错误 request_hash → 解密失败');

  // 命令构造解析回环：buildGetVehicleData → parseRoutable
  const cmdBody = pb.buildGetVehicleData({
    ciphertext: sealed.ciphertext, epoch: epoch, nonce: sealed.nonce,
    counter: sealed.counter, expiresAt: sealed.expiresAt, tag: sealed.tag,
    routing: new Uint8Array([1, 2, 3, 4]), pub: CLIENT_PUB, flags: 2,
    uuid: new Uint8Array(16),
  });
  const cmdRm = pb.parseRoutable(cmdBody);
  ok(cmdRm !== null && cmdRm.gcm !== null && cmdRm.toDomain === 3, '命令 RoutableMessage 回环');
  eq(cmdRm.gcm.counter, sealed.counter, '命令 gcm.counter');
  eq(cmdRm.flags, 2, '命令 flags');
}

// ── 5. CarServer.Response 解码 ────────────────────────────────────
console.log('5. CarServer.Response 解码');
{
  const plain = buildTestCarServerResponse();
  const dec = parseCarServerResponse(plain);
  ok(dec.ok === true && dec.vehicleData !== null, 'Response 可解析');
  const vd = dec.vehicleData;
  // speed_float=106 mph=60 → mph 值保持原始单位
  ok(vd.drive && vd.drive.speedMph === 60, 'drive.speedMph=60 (mph)');
  ok(vd.drive && vd.drive.powerKw === -10, 'drive.powerKw=-10（动能回收）');
  ok(vd.drive && vd.drive.gear === 'D', 'drive.gear=D');
  ok(vd.charge && vd.charge.charging === true, 'charge.charging=true (Charging=5)');
  ok(vd.charge && vd.charge.soc === 80, 'charge.soc=80');
  ok(vd.charge && Math.abs(vd.charge.rangeMiles - 150.5) < 0.01, 'charge.rangeMiles=150.5');
}

// ── 6. 配钥请求构造 ───────────────────────────────────────────────
console.log('6. add-key-request 构造');
{
  const body = pb.buildPairingRequest(CLIENT_PUB, 2, 9);
  const r = new pb.PbReader(body);
  let foundSigned = false;
  let foundSigType = false;
  while (!r.atEnd()) {
    const h = r.next();
    if (h.field === 1 && h.wire === 2) {
      foundSigned = true; // ToVCSECMessage.signedMessage
      const d = r.readBytes();
      const r2 = new pb.PbReader(d);
      while (!r2.atEnd()) {
        const h2 = r2.next();
        if (h2.field === 3 && h2.wire === 0) { foundSigType = r2.readVarint() === 2; }
        else r2.skip(h2.wire);
      }
    } else r.skip(h.wire);
  }
  ok(foundSigned, 'ToVCSECMessage.signedMessage 存在');
  ok(foundSigType, 'signatureType = PRESENT_KEY(2)');
}

// ── 辅助：构造测试 CarServer.Response ─────────────────────────────
function buildTestCarServerResponse() {
  // DriveState{shift_state=1{5:Charging?'D'=5}, power=103=-10, speed_float=106=60mph}
  const shift = new pb.PbWriter();
  shift.putEmptyMessage(5); // 'D'
  const drive = new pb.PbWriter();
  drive.putMessage(1, shift);
  drive.putVarint(103, (-10) >>> 0);
  drive.putFloat32(106, 60); // speed_float mph
  // ChargeState{charging_state=1{5:Charging}, battery_range=111=150.5, battery_level=114=80}
  const charging = new pb.PbWriter();
  charging.putEmptyMessage(5);
  const charge = new pb.PbWriter();
  charge.putMessage(1, charging);
  const dv = new DataView(new ArrayBuffer(4));
  dv.setFloat32(0, 150.5, true);
  charge.putFloat32(111, 150.5); // battery_range miles
  charge.putVarint(114, 80);
  // VehicleData{charge_state=3, drive_state=5}
  const vd = new pb.PbWriter();
  vd.putMessage(3, charge);
  vd.putMessage(5, drive);
  // Response{vehicleData=2}
  const resp = new pb.PbWriter();
  resp.putMessage(2, vd);
  return resp.finish();
}

// ── 汇总 ──────────────────────────────────────────────────────────
console.log('');
console.log('M4 自测: ' + passed + ' 通过, ' + failed + ' 失败');
process.exit(failed ? 1 : 0);
