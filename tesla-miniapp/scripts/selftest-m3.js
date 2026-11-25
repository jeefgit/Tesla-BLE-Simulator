// M3 自测：53B 编解码回环 + 组帧器跨块重组 + UI 模型降级（node scripts/selftest-m3.js）
'use strict';

const assert = require('assert');

const { encodeSimPayload, decodeSimPayload } = require('../core/teslaPayload');
const { FrameParser, MODE_PLAIN, MODE_ENCRYPTED } = require('../core/frameParser');
const { VehicleState } = require('../core/vehicleState');
const uiModel = require('../core/uiModel');

function chunk(bytes, size) {
  const out = [];
  for (let off = 0; off < bytes.length; off += size) out.push(bytes.subarray(off, off + size));
  return out;
}

let passed = 0;

// ── 53B 编解码回环 ────────────────────────────────────────────────
const sample = {
  speedKmh: 87.5, motorPowerKw: -23.25, batteryLevel: 62, batteryRangeKm: 248.0,
  gear: 'D', doors: [false, true, false, false, true, false], locked: true,
  charging: false, chargePowerKm: undefined, chargePowerKw: 0.0,
  insideTemp: 21.5, outsideTemp: 14.25,
  tpms: [2.5, 2.6, 2.4, 2.5],
};
const enc1 = encodeSimPayload(sample);
assert.strictEqual(enc1.length, 53, 'payload length 53');
const dec1 = decodeSimPayload(enc1);
assert.ok(dec1, 'decode ok');
assert.strictEqual(dec1.gear, 'D', 'gear');
assert.strictEqual(dec1.batteryLevel, 62, 'soc int');
assert.strictEqual(dec1.locked, true, 'locked');
assert.strictEqual(dec1.charging, false, 'charging');
assert.deepStrictEqual(dec1.doors, [false, true, false, false, true, false], 'doors');
for (const [a, b] of [[sample.speedKmh, dec1.speedKmh], [sample.motorPowerKw, dec1.motorPowerKw],
  [sample.batteryRangeKm, dec1.batteryRangeKm], [sample.insideTemp, dec1.insideTemp],
  [sample.outsideTemp, dec1.outsideTemp]]) {
  assert.ok(Math.abs(a - b) < 1e-3, 'float roundtrip');
}
for (let i = 0; i < 4; i++) assert.ok(Math.abs(sample.tpms[i] - dec1.tpms[i]) < 1e-3, 'tpms ' + i);
passed += 10;
console.log('teslaPayload 53B encode/decode roundtrip OK');

// gear '?'（巡航）编码
const cruise = encodeSimPayload(Object.assign({}, sample, { gear: '?' }));
assert.strictEqual(decodeSimPayload(cruise).gear, '?', 'cruise gear');
passed += 1;

// ── 组帧器：plain 模式 ────────────────────────────────────────────
const p = new FrameParser();
p.setMode(MODE_PLAIN);
// 整帧到达
let frames = p.feed(enc1);
assert.strictEqual(frames.length, 1);
assert.strictEqual(frames[0].type, 'sim');
passed += 2;
// 20B 分片跨通知边界
p.reset();
frames = [];
for (const c of chunk(enc1, 20)) frames = frames.concat(p.feed(c));
assert.strictEqual(frames.length, 1, 'chunked plain reassembly');
assert.strictEqual(frames[0].type, 'sim');
passed += 1;
// 背靠背两帧
p.reset();
const two = new Uint8Array(106);
two.set(enc1, 0);
two.set(enc1, 53);
frames = p.feed(two);
assert.strictEqual(frames.length, 2, 'two frames one chunk');
frames = frames.concat(p.feed(new Uint8Array(0)));
assert.strictEqual(frames.length, 2);
passed += 2;
console.log('frameParser plain mode OK (chunked / back-to-back)');

// ── 组帧器：encrypted 模式 ────────────────────────────────────────
const e = new FrameParser();
e.setMode(MODE_ENCRYPTED);
function encFrame(payload) {
  const out = new Uint8Array(payload.length + 2);
  out[0] = (payload.length >> 8) & 0xff;
  out[1] = payload.length & 0xff;
  out.set(payload, 2);
  return out;
}
const ep = new Uint8Array(40);
for (let i = 0; i < 40; i++) ep[i] = i;
const ef = encFrame(ep);
// 跨 3 块重组
frames = [];
frames = frames.concat(e.feed(ef.subarray(0, 10)));
assert.strictEqual(frames.length, 0, 'no frame before complete');
frames = frames.concat(e.feed(ef.subarray(10, 30)));
assert.strictEqual(frames.length, 0);
frames = frames.concat(e.feed(ef.subarray(30)));
assert.strictEqual(frames.length, 1);
assert.strictEqual(frames[0].type, 'enc');
assert.strictEqual(frames[0].data.length, 40);
passed += 3;

// 未握手时 53B 形状噪声帧丢弃（前缀恰好构成 declared=51 的确定性样例）
const noise53 = new Uint8Array(53);
noise53[1] = 0x33; // declared = 0x0033 = 51
const e2 = new FrameParser();
e2.setMode(MODE_ENCRYPTED);
frames = e2.feed(noise53);
assert.strictEqual(frames.length, 0, 'noise dropped pre-handshake');
assert.strictEqual(e2.buffered, 0, 'noise fully consumed');
// 握手成功后同形状 53B 帧（51B 载荷）是合法加密帧
e2.setHandshakeDone(true);
noise53[2] = 0xaa;
frames = e2.feed(noise53);
assert.strictEqual(frames.length, 1, 'legit 53B enc frame after handshake');
assert.strictEqual(frames[0].data[0], 0xaa);
passed += 4;

// 混流：两帧 + 半帧
const e3 = new FrameParser();
e3.setMode(MODE_ENCRYPTED);
e3.setHandshakeDone(true);
const ef53 = encFrame(new Uint8Array(51)); // 总长 53 的合法加密帧
const mixed = new Uint8Array(ef.length + ef53.length + ef.subarray(0, 5).length);
mixed.set(ef, 0);
mixed.set(ef53, ef.length);
mixed.set(ef.subarray(0, 5), ef.length + ef53.length);
frames = e3.feed(mixed);
assert.strictEqual(frames.length, 2, 'mixed stream');
assert.strictEqual(e3.buffered, 5, 'partial remains buffered');
frames = frames.concat(e3.feed(ef.subarray(5)));
assert.strictEqual(frames.length, 3, 'partial completed');
passed += 3;
console.log('frameParser encrypted mode OK (reassembly / noise / handshake flag)');

// ── UI 模型降级 ───────────────────────────────────────────────────
const vs = new VehicleState();
let m = uiModel.build(vs.get());
assert.strictEqual(m.page, 'link', 'link page when not ready');
assert.strictEqual(m.drive.speedText, '--', 'degraded speed');
passed += 2;

vs.setLink('ready', '');
m = uiModel.build(vs.get());
assert.strictEqual(m.page, 'drive', 'drive page when ready but no data');
assert.strictEqual(m.drive.speedText, '--', 'no data -> --');
assert.strictEqual(m.doors.unknown, true, 'doors unknown');
passed += 3;

vs.applySimPayload(Object.assign({}, sample, { charging: true }));
m = uiModel.build(vs.get());
assert.strictEqual(m.page, 'charge', 'charge page');
assert.strictEqual(m.charge.socText, '62', 'soc text');
assert.strictEqual(m.doors.anyOpen, true, 'doors overlay');
assert.strictEqual(m.tpms.values[0].text, '2.50', 'tpms text');
passed += 3;

console.log('uiModel degradation / page state machine OK');
console.log('\nM3 SELFTESTS PASSED (' + passed + ' assertions)');
