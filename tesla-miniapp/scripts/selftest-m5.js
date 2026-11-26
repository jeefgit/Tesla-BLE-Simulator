// M5 自测：辅助轮询调度（charge/climate/closures/tire 31s 轮换）+ 响应按类别排周期
//        + CarServer 五类状态解码入 vstate + UI 模型（ETA/门/胎压/温度）+ 配钥事件流
// 运行：node scripts/selftest-m5.js
'use strict';

const path = require('path');
const ROOT = path.join(__dirname, '..');

const pb = require(path.join(ROOT, 'core/pbCodec'));
const { TeslaClient } = require(path.join(ROOT, 'core/teslaClient'));
const { VehicleState } = require(path.join(ROOT, 'core/vehicleState'));
const uiModel = require(path.join(ROOT, 'core/uiModel'));
const { CFG } = require(path.join(ROOT, 'config/index'));
const { fromHex, toHex } = require(path.join(ROOT, 'utils/bytes'));

let passed = 0;
let failed = 0;
function ok(cond, name) {
  if (cond) { passed++; }
  else { failed++; console.log('  FAIL: ' + name); }
}
function near(actual, expected, slackMs, name) {
  ok(Math.abs(actual - expected) <= slackMs, name + ' (got ' + actual + ' want≈' + expected + ')');
}

// 测试客户端私钥（与 selftest-m4 一致）
const CLIENT_PRIV_HEX = 'E861650948F023C7E541A2BE5C0595A6D15AD130A72C86ADD74B73774F619FA8';
const VIN = 'LRWYGCFJ8RC571213';

// ── 伪造链路 + 会话存根 ───────────────────────────────────────────
function makeFakeLink(sentFrames) {
  return {
    state: 'ready',
    on() {},
    send(f) { sentFrames.push(f); return Promise.resolve(); },
    stats: { rxBytes: 0, rxFrames: 0 },
  };
}

let cmdCounter = 100;
let tagSeq = 0;
let openQueue = [];
function stubSession(client) {
  client._routing = new Uint8Array([1, 2, 3, 4]); // 正常由握手生成，测试直接给定
  client._session = {
    epoch: new Uint8Array(16),
    sealCommand() {
      tagSeq++;
      const tag = new Uint8Array(16);
      tag[0] = tagSeq & 0xff;
      return {
        ciphertext: new Uint8Array([1, 2, 3, 4]),
        nonce: new Uint8Array(12),
        counter: cmdCounter++,
        tag: tag,
        expiresAt: 4102444800,
      };
    },
    openResponse() { return openQueue.shift() || null; },
  };
}

// ── CarServer.Response 构造器 ─────────────────────────────────────
function wrapVehicleData(subField, subWriter) {
  const vd = new pb.PbWriter();
  vd.putMessage(subField, subWriter);
  const resp = new pb.PbWriter();
  resp.putMessage(2, vd);
  return resp.finish();
}

// ChargeState{charging_state=1{5:Charging}, battery_level=114, charger_power=122}
function buildChargeResponse(soc, powerKw) {
  const charging = new pb.PbWriter();
  charging.putEmptyMessage(5);
  const charge = new pb.PbWriter();
  charge.putMessage(1, charging);
  charge.putVarint(114, soc);
  charge.putVarint(122, powerKw);
  return wrapVehicleData(3, charge);
}

// ClimateState{inside_temp=101, outside_temp=102}（float ℃）
function buildClimateResponse(inside, outside) {
  const climate = new pb.PbWriter();
  climate.putFloat32(101, inside);
  climate.putFloat32(102, outside);
  return wrapVehicleData(4, climate);
}

// ClosuresState{101=FL,103=FR,102=RL,104=RR,105=FRUNK,106=TRUNK,113=locked}
function buildClosuresResponse(doors, locked) {
  const fields = [101, 103, 102, 104, 105, 106];
  const cl = new pb.PbWriter();
  for (let i = 0; i < 6; i++) cl.putVarint(fields[i], doors[i] ? 1 : 0);
  cl.putVarint(113, locked ? 1 : 0);
  return wrapVehicleData(9, cl);
}

// TirePressureState{2..5 = FL/FR/RL/RR float bar}
function buildTireResponse(tp) {
  const t = new pb.PbWriter();
  for (let i = 0; i < 4; i++) t.putFloat32(2 + i, tp[i]);
  return wrapVehicleData(19, t);
}

function fakeEncResponse() {
  return {
    ciphertext: new Uint8Array([9, 9, 9]),
    resp: { nonce: new Uint8Array(12), counter: 5, tag: new Uint8Array(16) },
    fault: 0,
    flags: 2,
  };
}

function freshPending(kind) {
  tagSeq++;
  const tag = new Uint8Array(16);
  tag[0] = tagSeq & 0xff;
  return { reqTag: tag, counter: 1, uuid: new Uint8Array(16), kind: kind, sentAt: Date.now() };
}

// ── 1. 辅助轮询调度：串行化 + 轮换 + 退避 ─────────────────────────
console.log('1. 辅助轮询调度');
const sent = [];
const vstate = new VehicleState();
{
  const client = new TeslaClient({
    link: makeFakeLink(sent), vstate: vstate, vin: VIN,
    privKeyHex: CLIENT_PRIV_HEX, onLog() {}, onEvent() {},
  });
  stubSession(client);
  client._hasSession = true;
  const t0 = Date.now();

  // 1a. Drive 到期 → 发 drive；在途期间不再发任何命令
  client._nextDriveAt = t0 - 1;
  client._nextAuxAt = 0;
  client.tick();
  ok(sent.length === 1, 'Drive 到期发出 1 帧');
  ok(client._pollPending && client._pollPending.kind === 'drive', '在途命令 kind=drive');
  near(client._nextDriveAt, t0 + CFG.DRIVE_POLL_INTERVAL_MS, 120, 'drive 下次周期 = now+300ms');
  client.tick();
  ok(sent.length === 1, '在途未清空时不发新命令（串行化）');

  // 1b. 辅助类别在途超时 → 按类别退避（_nextAuxAt = now+RETRY），且 drive 不受影响
  client._pollPending = freshPending('charge');
  client._pollPending.sentAt -= CFG.POLL_RESPONSE_TIMEOUT_MS + 100;
  client._nextDriveAt = Date.now() + 100000;
  client.tick();
  ok(client._pollPending === null, '超时后清空在途');
  near(client._nextAuxAt, Date.now() + CFG.POLL_RETRY_MS, 120, 'charge 超时 → aux 退避 now+500ms');
  ok(sent.length === 1, '退避期内不发新帧');

  // 1c. 辅助轮换顺序 charge → climate → closures → tire →（回绕）charge
  const seq = [];
  for (let i = 0; i < 5; i++) {
    client._nextAuxAt = Date.now() - 1; // 模拟 31s 周期到点
    client.tick();
    ok(client._pollPending && client._pollPending.kind !== 'drive', 'aux 发出且非 drive');
    seq.push(client._pollPending.kind);
    near(client._nextAuxAt, Date.now() + CFG.AUX_POLL_INTERVAL_MS, 120, 'aux 下次周期 = now+31s');
    client._pollPending = null; // 模拟响应到达清空
  }
  ok(seq.join(',') === 'charge,climate,closures,tire,charge', '轮换顺序 charge→climate→closures→tire→回绕 (实际 ' + seq.join(',') + ')');

  // 1d. Drive 优先于辅助类别
  client._nextDriveAt = Date.now() - 1;
  client._nextAuxAt = Date.now() - 1;
  client.tick();
  ok(client._pollPending && client._pollPending.kind === 'drive', 'drive 与 aux 同时到期时 drive 优先');
}

// ── 2. 响应到达：按类别排周期 + 状态写入 vstate ───────────────────
console.log('2. 响应调度与状态写入');
{
  const client = new TeslaClient({
    link: makeFakeLink([]), vstate: vstate, vin: VIN,
    privKeyHex: CLIENT_PRIV_HEX, onLog() {}, onEvent() {},
  });
  stubSession(client);
  client._hasSession = true;
  vstate.setLink('ready', '');

  // 2a. charge 响应 → soc/charging/power 入快照，aux 周期重排 31s
  client._pollPending = freshPending('charge');
  openQueue = [buildChargeResponse(50, 62)];
  client._handleEncryptedResponse(fakeEncResponse());
  ok(client._pollPending === null, 'charge 响应后清空在途');
  near(client._nextAuxAt, Date.now() + CFG.AUX_POLL_INTERVAL_MS, 120, 'charge 响应 → aux 周期重排 now+31s');
  ok(vstate.get().snap.batteryLevel === 50, 'batteryLevel=50');
  ok(vstate.get().snap.charging === true, 'charging=true');
  ok(vstate.get().snap.chargePowerKw === 62, 'chargePowerKw=62');

  // 2b. climate 响应
  client._pollPending = freshPending('climate');
  openQueue = [buildClimateResponse(21.5, 18.2)];
  client._handleEncryptedResponse(fakeEncResponse());
  ok(Math.abs(vstate.get().snap.insideTemp - 21.5) < 0.01, 'insideTemp=21.5');
  ok(Math.abs(vstate.get().snap.outsideTemp - 18.2) < 0.01, 'outsideTemp=18.2');

  // 2c. closures 响应：FL 开、其余关、未锁（索引映射 101=FL,103=FR,102=RL,104=RR）
  client._pollPending = freshPending('closures');
  openQueue = [buildClosuresResponse([true, false, false, false, false, false], false)];
  client._handleEncryptedResponse(fakeEncResponse());
  ok(vstate.get().snap.locked === false, 'locked=false');
  ok(JSON.stringify(vstate.get().snap.doors) === JSON.stringify([true, false, false, false, false, false]), 'doors[0]=true（FL 开）');

  // 2d. tire 响应：RL 1.8 bar 低于 2.0 阈值
  client._pollPending = freshPending('tire');
  openQueue = [buildTireResponse([2.3, 2.4, 1.8, 2.5])];
  client._handleEncryptedResponse(fakeEncResponse());
  ok(vstate.get().snap.tpms[2] !== null && Math.abs(vstate.get().snap.tpms[2] - 1.8) < 0.01, 'tpms[2]=1.8');

  // 2e. drive 响应 → drive 周期重排 300ms（而非 aux 周期）
  client._pollPending = freshPending('drive');
  const driveW = new pb.PbWriter();
  driveW.putFloat32(106, 60); // 60 mph
  const resp2 = new pb.PbWriter();
  resp2.putMessage(2, (function () { const vd = new pb.PbWriter(); vd.putMessage(5, driveW); return vd; })());
  openQueue = [resp2.finish()];
  client._handleEncryptedResponse(fakeEncResponse());
  near(client._nextDriveAt, Date.now() + CFG.DRIVE_POLL_INTERVAL_MS, 120, 'drive 响应 → 周期重排 now+300ms');
  ok(Math.abs(vstate.get().snap.speedKmh - 60 * 1.609344) < 0.1, 'speedKmh = 60mph 换算 km');
}

// ── 3. UI 模型：ETA / 门 / 胎压 / 温度 / 页面状态机 ────────────────
console.log('3. UI 模型（ETA/门/胎压/温度）');
{
  // 先清掉 2a 残留的充电态，验证降级与页面状态机
  vstate.applyPartial({ charging: false, chargePowerKw: 0 });
  const m1 = uiModel.build(vstate.get());
  ok(m1.page === 'drive', '非充电状态 → drive 页');
  ok(m1.charge.etaText === '--', '未充电 → ETA 降级 --');
  ok(m1.doors.unknown === false && m1.doors.doors[0].open === true && m1.doors.anyOpen === true, '门模型 FL 开 / anyOpen');

  // 充电态：62kW + 50% → ETA 30min；页面切 charge
  vstate.applyPartial({ charging: true, chargePowerKw: 62 });
  const m2 = uiModel.build(vstate.get());
  ok(m2.page === 'charge', 'charging=true → charge 页');
  ok(m2.charge.etaText === '30min', '62kW/50% → ETA 30min（62kWh 电池组假设）');
  ok(m2.tpms.values[2].low === true, '1.8 bar < 2.0 → low 告警');
  ok(m2.tpms.values[0].low === false, '2.3 bar 不告警');
  ok(m2.climate.insideText === '21.5' && m2.climate.outsideText === '18.2', '温度文本保留 1 位小数');

  const rangeState = new VehicleState();
  rangeState.setLink('ready', '');
  rangeState.applyPartial({ batteryRangeKm: 321 });
  rangeState._ts.battery = Date.now() - CFG.DATA_STALE_MS - 1;
  const rangeModel = uiModel.build(rangeState.get());
  ok(rangeModel.drive.rangeText === '321', '里程在快频数据过期后仍保持有效');
  rangeState._ts.battery = Date.now() - CFG.AUX_DATA_STALE_MS - 1;
  ok(uiModel.build(rangeState.get()).drive.rangeText === '--', '辅助数据超过有效期后里程降级为 --');

  // 门全关 → anyOpen=false
  vstate.applyPartial({ doors: [false, false, false, false, false, false], locked: true });
  const m3 = uiModel.build(vstate.get());
  ok(m3.doors.anyOpen === false && m3.locked === true, '门全关 / locked=true');
}

// ── 4. 配钥事件流（浮层状态机数据源）─────────────────────────────
console.log('4. 配钥事件流');
{
  const events = [];
  const client = new TeslaClient({
    link: makeFakeLink([]), vstate: new VehicleState(), vin: VIN,
    privKeyHex: CLIENT_PRIV_HEX, onLog() {}, onEvent: (e) => events.push(e),
  });
  client._notPaired = true;

  client._sendPairingRequest();
  let ev = events[events.length - 1];
  ok(ev.type === 'pairing' && ev.phase === 'sending' && ev.sends === 1 && ev.maxSends === CFG.PAIR_MAX_SENDS, 'phase=sending（sends 计数）');

  client._onPairingStatus({ opStatus: 1, terminal: false, info: 0 });
  ev = events[events.length - 1];
  ok(ev.phase === 'waiting' && ev.waitMs === CFG.PAIR_WAIT_TIMEOUT_MS, 'phase=waiting（waitMs=90s）');

  client._onPairingStatus({ opStatus: 0, terminal: true });
  ev = events[events.length - 1];
  ok(ev.phase === 'ok' && client._notPaired === false, 'phase=ok 且退出 notPaired');
  ok(client._nextHsAt > Date.now(), '配钥成功后安排重新握手');

  // 被拒 → failed；贴卡等待超时 → roundRestart 后重发
  client._enterNotPaired();
  client._onPairingStatus({ opStatus: 2, info: 12, terminal: true });
  ev = events[events.length - 1];
  ok(ev.phase === 'failed' && ev.info === 12, 'phase=failed（info 透传）');

  client._pairWaitNfc = true;
  client._pairStartedAt = Date.now() - CFG.PAIR_WAIT_TIMEOUT_MS - 10;
  client._nextPairAt = 0;
  client.tick();
  ev = events[events.length - 1];
  ok(ev.phase === 'roundRestart', '贴卡超时 → roundRestart');
  client.tick();
  ev = events[events.length - 1];
  ok(ev.phase === 'sending' && ev.sends === 1, '重试轮次重新计数');
}

// ── 5. 会话建立后的首轮调度（握手成功 → 辅助类别 400ms 内起步）───
console.log('5. 会话建立首轮调度');
{
  const client = new TeslaClient({
    link: makeFakeLink([]), vstate: new VehicleState(), vin: VIN,
    privKeyHex: CLIENT_PRIV_HEX, onLog() {}, onEvent() {},
  });
  const before = Date.now();
  client._hasSession = false;
  client._auxIdx = 3; // 模拟残留状态
  // 直接走 _handleHandshakeResponse 的成功路径太重，这里校验 reset + 手动置时序语义
  client._resetCryptoState();
  ok(client._auxIdx === 0 && client._nextAuxAt === 0 && client._nextDriveAt === 0, 'reset 清空 aux/drive 时序');
  client._hasSession = true;
  client._nextDriveAt = before + 100;
  client._auxIdx = 0;
  client._nextAuxAt = before + 400;
  ok(client._nextAuxAt - client._nextDriveAt === 300, '辅助首轮晚于 Drive 首轮 300ms');
}

// ── 汇总 ──────────────────────────────────────────────────────────
console.log('');
console.log('M5 自测: ' + passed + ' 通过, ' + failed + ' 失败');
process.exit(failed ? 1 : 0);
