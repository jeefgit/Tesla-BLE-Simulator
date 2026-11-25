// TeslaClient：加密握手状态机 + 串行化数据轮询 + fault 处理 + 明文配钥
// 移植自 ESP32 TeslaClient.cpp / TeslaCrypto.cpp，接入 BleLink 与 VehicleState
//
// 链路契约（硬约束）：
// - 握手成功前丢弃所有非握手帧（53B 旁路帧按噪声处理，计满即自动回退 plain 模式）
// - 命令串行化：同一时刻只允许一条加密命令在途（pollPending）
// - 写特征一律带响应写（BleLink.send 内部保证）
'use strict';

const { CFG } = require('../config/index');
const { STATE } = require('./bleLink');
const { FrameParser, MODE_PLAIN, MODE_ENCRYPTED } = require('./frameParser');
const { VehicleState } = require('./vehicleState');
const { decodeSimPayload } = require('./teslaPayload');
const { parseCarServerResponse } = require('./carServer');
const sessionCrypto = require('./crypto/session');
const crypto = require('./crypto/index');
const pb = require('./pbCodec');
const { fromHex, toHex } = require('../utils/bytes');

const MILES_TO_KM = 1.609344;
const TICK_MS = 100;

// UniversalMessage.MessageFault_E（universal_message.proto）
const FAULT_NONE = 0;
const FAULT_UNKNOWN_KEY_ID = 3;      // 本机公钥不在车端白名单（未配对）
const FAULT_INACTIVE_KEY = 4;
const FAULT_INVALID_SIGNATURE = 5;
const FAULT_INVALID_TOKEN_OR_COUNTER = 6;
const FAULT_INCORRECT_EPOCH = 15;
const FAULT_TIME_EXPIRED = 17;
const FAULT_RESPONSE_MTU_EXCEEDED = 25;
// 这些 fault 表示会话失步，需要重新握手
const DESYNC_FAULTS = [FAULT_INVALID_SIGNATURE, FAULT_INVALID_TOKEN_OR_COUNTER,
  FAULT_INCORRECT_EPOCH, FAULT_TIME_EXPIRED];

// VCSEC.OperationStatus_E
const OP_WAIT = 1;
const OP_OK = 0;
const OP_ERROR = 2;

const FAULT_NAMES = {
  1: 'BUSY', 2: 'TIMEOUT', 3: 'UNKNOWN_KEY_ID(未配对)', 4: 'INACTIVE_KEY',
  5: 'INVALID_SIGNATURE', 6: 'COUNTER_USED', 7: 'NO_PRIVILEGES', 8: 'INVALID_DOMAINS',
  9: 'INVALID_COMMAND', 10: 'DECODING', 15: 'INCORRECT_EPOCH', 17: 'TIME_EXPIRED',
  25: 'RESPONSE_MTU_EXCEEDED',
};

function nowMs() { return Date.now(); }

class TeslaClient {
  // opts: { link, vstate, vin, privKeyHex, onLog, onEvent }
  constructor(opts) {
    this._link = opts.link;
    this._vstate = opts.vstate || new VehicleState();
    this._vin = opts.vin;
    this._onLog = opts.onLog || function () {};
    this._onEvent = opts.onEvent || function () {};

    // 客户端密钥
    const priv = fromHex(opts.privKeyHex || '');
    this._priv = priv.length === 32 ? priv : null;
    this._pub = this._priv ? crypto.derivePublicKey(this._priv) : null;

    this._session = new sessionCrypto.TeslaSession(this._vin);

    // 组帧器：加密模式起步（握手响应用 2B 长度前缀帧），握手成功前置 false
    this._parser = new FrameParser();
    this._parser.setMode(MODE_ENCRYPTED);
    this._parser.setHandshakeDone(false);
    this._plainMode = false; // 检测到纯旁路模拟器后回退 plain
    this._noiseCount = 0;
    this._parser.onDropped = () => { this._noiseCount++; };

    // 状态机
    this._hasSession = false;
    this._hsTries = 0;
    this._nextHsAt = 0;
    this._challenge = null;
    this._routing = null;

    this._pollPending = null; // { reqTag, counter, uuid, kind, sentAt }
    this._nextDriveAt = 0;
    // 辅助类别轮换：charge → climate → closures → tire，31s 一个类别
    this._auxKinds = ['charge', 'climate', 'closures', 'tire'];
    this._auxIdx = 0;
    this._nextAuxAt = 0;

    this._notPaired = false;
    this._pairSends = 0;
    this._pairStartedAt = 0;
    this._nextPairAt = 0;
    this._pairWaitNfc = false;
    this._pairWaitLogged = false;

    this.stats = { hsSent: 0, hsOk: 0, cmdsSent: 0, respOk: 0, respFail: 0,
      timeouts: 0, decodeFail: 0, dropped: 0, noise: 0 };
    this._lastFaultLogAt = 0;
    this._lastDropLogAt = 0;
    this._timer = null;
    this._started = false;
  }

  get hasSession() { return this._hasSession; }

  // ── 生命周期 ──────────────────────────────────────────────────────
  start() {
    if (this._started) return;
    this._started = true;
    this._link.on('state', (s) => this._onLinkState(s));
    this._link.on('bytes', (b) => this.onBytes(b));
    this._timer = setInterval(() => this.tick(), TICK_MS);
    if (!this._priv) {
      this._log('私钥未配置（config/secret.js），加密握手不可用');
      this._emit({ type: 'error', message: 'NO_KEY' });
    }
  }

  stop() {
    if (this._timer) clearInterval(this._timer);
    this._timer = null;
    this._started = false;
  }

  _onLinkState(s) {
    if (s.state === STATE.READY) {
      this._resetCryptoState();
      this._nextHsAt = nowMs() + 300;
    } else {
      // 连接建立前/断开/重连中：清会话与在途命令
      this._resetCryptoState();
    }
  }

  _resetCryptoState() {
    this._hasSession = false;
    this._session.reset();
    this._pollPending = null;
    this._parser.setHandshakeDone(false);
    this._hsTries = 0;
    this._nextHsAt = 0;
    this._auxIdx = 0;
    this._nextAuxAt = 0;
    this._notPaired = false;
    this._pairSends = 0;
    this._pairWaitNfc = false;
    this._pairStartedAt = 0;
    // plain 回退只在同一次连接内判定，断线后重新走加密握手
    if (this._plainMode) {
      this._plainMode = false;
      this._noiseCount = 0;
      this._parser.setMode(MODE_ENCRYPTED);
    }
  }

  // ── 字节入口 ──────────────────────────────────────────────────────
  onBytes(bytes) {
    const frames = this._parser.feed(bytes);
    for (let i = 0; i < frames.length; i++) {
      const f = frames[i];
      if (f.type === 'sim') {
        const d = decodeSimPayload(f.data);
        if (d) this._vstate.applySimPayload(d);
      } else if (f.type === 'enc') {
        this._handleEncFrame(f.data);
      }
    }
  }

  // ── 主循环 ────────────────────────────────────────────────────────
  tick() {
    if (this._link.state !== STATE.READY) return;
    const now = nowMs();

    if (!this._hasSession) {
      // 纯旁路模拟器探测：握手连败 3 次 + 噪声帧 ≥3 → 回退 plain（M3 行为）
      if (!this._plainMode && !this._notPaired && this._hsTries >= CFG.HANDSHAKE_MAX_RETRY
        && this._noiseCount >= 3) {
        this._plainMode = true;
        this._parser.setMode(MODE_PLAIN);
        this._log('未检测到加密端点（53B 旁路噪声），回退 plain 模式');
        this._emit({ type: 'plainFallback' });
      }
      if (this._plainMode) return;

      // 配钥流程：add-key-request 8s×3，WAIT 后纯等 90s
      if (this._notPaired && CFG.PAIR_ON_DENIED) {
        if (!this._priv) return;
        const timedOut = now - this._pairStartedAt > CFG.PAIR_WAIT_TIMEOUT_MS;
        if (this._pairWaitNfc) {
          if (timedOut) {
            this._pairWaitNfc = false;
            this._pairWaitLogged = false;
            this._pairSends = 0;
            this._pairStartedAt = now;
            this._nextPairAt = now;
            this._log('等待刷卡超时，重新发起 add-key-request');
            this._emit({ type: 'pairing', phase: 'roundRestart' });
          }
          return;
        }
        if (this._pairSends < CFG.PAIR_MAX_SENDS) {
          if (now >= this._nextPairAt) this._sendPairingRequest();
          return;
        }
        if (timedOut) {
          this._pairSends = 0;
          this._pairStartedAt = now;
          this._nextPairAt = now;
          this._log('未收到车端配钥响应，重新尝试');
          this._emit({ type: 'pairing', phase: 'roundRestart' });
        }
        return;
      }

      // 握手：2s 重试 ×3，之后 15s 慢速重试
      if (this._nextHsAt && now >= this._nextHsAt) this._sendHandshake();
      return;
    }

    // 加密轮询：在途超时 → 清理并按类别退避
    if (this._pollPending && now - this._pollPending.sentAt > CFG.POLL_RESPONSE_TIMEOUT_MS) {
      const kind = this._pollPending.kind;
      this._pollPending = null;
      this.stats.timeouts++;
      this._scheduleNextPoll(kind, CFG.POLL_RETRY_MS);
      this._logThrottled('轮询响应超时（' + this.stats.timeouts + ' 次）', 3000);
    }
    if (!this._pollPending) {
      // Drive 高频轮询优先；辅助类别（charge/climate/closures/tire）31s 轮换
      if (now >= this._nextDriveAt) {
        this._sendPoll('drive');
      } else if (this._nextAuxAt && now >= this._nextAuxAt) {
        this._sendPoll(this._auxKinds[this._auxIdx]);
      }
    }
  }

  // ── 握手 ──────────────────────────────────────────────────────────
  _sendHandshake() {
    if (!this._priv) return;
    this._hsTries++;
    this.stats.hsSent++;
    this._challenge = sessionCrypto.fillRandom(16);
    this._routing = sessionCrypto.fillRandom(4);
    const body = pb.buildHandshakeRequest(this._pub, this._routing, this._challenge);
    const delay = this._hsTries <= CFG.HANDSHAKE_MAX_RETRY
      ? CFG.HANDSHAKE_RETRY_MS : CFG.HANDSHAKE_RETRY_LONG_MS;
    this._nextHsAt = nowMs() + delay;
    this._log('发送握手请求 #' + this._hsTries);
    this._send(pb.frameOf(body)).catch((e) => this._log('握手发送失败: ' + e.message));
  }

  _handleHandshakeResponse(frame, rm) {
    const res = this._session.completeHandshake(
      this._priv, this._challenge, rm.sessionInfo, rm.sessionInfoTag);
    if (res.ok) {
      this._hasSession = true;
      this._hsTries = 0;
      this.stats.hsOk++;
      this._parser.setHandshakeDone(true);
      this._pollPending = null;
      this._nextDriveAt = nowMs() + 100;
      this._auxIdx = 0;
      this._nextAuxAt = nowMs() + 400; // 会话建立后稍作停顿即发起首个辅助类别
      this._log('握手成功 counter=' + res.counter + ' K=' + (res.keyFingerprint || 0).toString(16));
      this._emit({ type: 'session', ok: true });
    } else if (res.notPaired) {
      this._log(res.reason);
      this._enterNotPaired();
    } else {
      this._log('握手失败: ' + res.reason);
      this._emit({ type: 'session', ok: false, reason: res.reason });
    }
  }

  _enterNotPaired() {
    this._notPaired = true;
    this._pairSends = 0;
    this._pairStartedAt = nowMs();
    this._nextPairAt = nowMs() + 200;
    this._pairWaitNfc = false;
    this._emit({ type: 'notPaired' });
  }

  // ── 配钥（明文 add-key-request）───────────────────────────────────
  _sendPairingRequest() {
    if (!this._priv) return;
    this._pairSends++;
    this._nextPairAt = nowMs() + CFG.PAIR_RETRY_MS;
    const body = pb.buildPairingRequest(this._pub, CFG.PAIR_ROLE, CFG.PAIR_FORM_FACTOR);
    this._log('发送 add-key-request #' + this._pairSends + '/' + CFG.PAIR_MAX_SENDS);
    this._emit({ type: 'pairing', phase: 'sending',
      sends: this._pairSends, maxSends: CFG.PAIR_MAX_SENDS });
    this._send(pb.frameOf(body)).catch((e) => this._log('配钥发送失败: ' + e.message));
  }

  _onPairingStatus(st) {
    if (st.opStatus === OP_WAIT && !st.terminal) {
      if (!this._pairWaitLogged) {
        this._pairWaitLogged = true;
        this._pairWaitNfc = true;
        this._log('车端等待刷卡确认（90s 内轻点钥匙卡）');
        this._emit({ type: 'pairing', op: st.opStatus, info: st.info, terminal: false,
          phase: 'waiting', waitMs: CFG.PAIR_WAIT_TIMEOUT_MS });
      }
      return;
    }
    if (st.terminal || st.opStatus === OP_OK || st.opStatus === OP_ERROR) {
      if (st.opStatus === OP_OK) {
        this._log('配钥成功，重新握手');
        this._notPaired = false;
        this._pairWaitNfc = false;
        this._nextHsAt = nowMs() + 300;
        this._emit({ type: 'pairing', op: OP_OK, info: 0, terminal: true, phase: 'ok' });
      } else {
        this._log('配钥被拒 opStatus=' + st.opStatus + ' info=' + st.info
          + '（info=12/5 表示公钥未授权，检查模拟器白名单）');
        // 保持 notPaired，允许按轮次重试
        this._pairWaitNfc = false;
        this._emit({ type: 'pairing', op: st.opStatus, info: st.info, terminal: true,
          phase: 'failed' });
      }
    }
  }

  // ── 加密轮询（drive 高频 + 辅助类别轮换，共用串行化在途保护）───────
  _sendPoll(kind) {
    if (!this._priv) return;
    const plain = pb.buildGetVehicleDataAction(kind);
    const sealed = this._session.sealCommand(plain, sessionCrypto.FLAG_ENCRYPT_RESPONSE);
    if (!sealed) return;
    const uuid = sessionCrypto.fillRandom(16);
    const body = pb.buildGetVehicleData({
      ciphertext: sealed.ciphertext,
      epoch: this._session.epoch,
      nonce: sealed.nonce,
      counter: sealed.counter,
      expiresAt: sealed.expiresAt,
      tag: sealed.tag,
      routing: this._routing,
      pub: this._pub,
      flags: sessionCrypto.FLAG_ENCRYPT_RESPONSE,
      uuid: uuid,
    });
    this._pollPending = {
      reqTag: sealed.tag,
      counter: sealed.counter,
      uuid: uuid,
      kind: kind,
      sentAt: nowMs(),
    };
    this.stats.cmdsSent++;
    // 发送即排下一周期（响应正常到达时由 _handleEncryptedResponse 覆盖为完整周期）
    if (kind === 'drive') {
      this._nextDriveAt = nowMs() + CFG.DRIVE_POLL_INTERVAL_MS;
    } else {
      this._nextAuxAt = nowMs() + CFG.AUX_POLL_INTERVAL_MS;
      this._auxIdx = (this._auxIdx + 1) % this._auxKinds.length;
      this._log('辅助轮询: ' + kind);
    }
    this._send(pb.frameOf(body)).catch((e) => {
      this._pollPending = null;
      this._log('轮询发送失败: ' + e.message);
    });
  }

  // ── 加密帧处理 ────────────────────────────────────────────────────
  _handleEncFrame(frame) {
    const rm = pb.parseRoutable(frame);
    if (!rm) {
      this.stats.dropped++;
      this._logThrottled('无法解析的 RoutableMessage（已丢 ' + this.stats.dropped + '）', 5000);
      return;
    }

    if (rm.sessionInfo) {
      if (!this._priv) return;
      this._handleHandshakeResponse(frame, rm);
      return;
    }

    if (!this._hasSession) {
      this._handlePreSession(rm);
      return;
    }

    // 已建立会话
    if (rm.fault !== FAULT_NONE) {
      this._handleFault(rm.fault);
      return;
    }

    if (rm.ciphertext && rm.resp) {
      this._handleEncryptedResponse(rm);
      return;
    }

    if (rm.ciphertext && rm.fromDomain === 2) {
      // VCSEC 明文：配钥终态可能晚于会话建立到达
      const st = pb.parsePairingStatus(rm.ciphertext);
      if (st && st.found) this._onPairingStatus(st);
      return;
    }

    if (rm.ciphertext) {
      // 老固件忽略 FLAG_ENCRYPT_RESPONSE：负载直接是明文 CarServer.Response
      this._applyVehicleData(rm.ciphertext);
    }
  }

  _handlePreSession(rm) {
    if (rm.fault !== FAULT_NONE) {
      this._logFault(rm.fault, '握手前');
      if (rm.fault === FAULT_UNKNOWN_KEY_ID || rm.fault === FAULT_INACTIVE_KEY) {
        this._enterNotPaired();
      }
      return;
    }
    if (rm.ciphertext && rm.fromDomain === 2) {
      // 配钥回包：FromVCSECMessage（明文）
      const st = pb.parsePairingStatus(rm.ciphertext);
      if (st && st.found) this._onPairingStatus(st);
      return;
    }
    // 其他握手前帧（普通 VCSEC 状态广播等）静默忽略
  }

  _handleFault(fault) {
    this._logFault(fault, '命令');
    const kind = this._pollPending ? this._pollPending.kind : 'drive';
    this._pollPending = null;
    if (fault === FAULT_UNKNOWN_KEY_ID || fault === FAULT_INACTIVE_KEY) {
      // 会话中途钥匙被移除等；进入配钥流程
      this._hasSession = false;
      this._parser.setHandshakeDone(false);
      this._enterNotPaired();
      return;
    }
    if (DESYNC_FAULTS.indexOf(fault) >= 0) {
      // 会话失步：重新握手
      this._hasSession = false;
      this._session.reset();
      this._parser.setHandshakeDone(false);
      this._nextHsAt = nowMs() + 500;
      this._emit({ type: 'session', ok: false, reason: FAULT_NAMES[fault] || ('fault=' + fault) });
      return;
    }
    this._scheduleNextPoll(kind, CFG.POLL_RETRY_MS);
    this._emit({ type: 'fault', fault });
  }

  _handleEncryptedResponse(rm) {
    const pending = this._pollPending;
    if (!pending) return; // 无在途命令的响应（重复响应）丢弃
    const resp = rm.resp;
    if (!resp.nonce || resp.nonce.length !== 12 || !resp.tag || resp.tag.length !== 16) {
      this._log('响应签名字段非法');
      this.stats.respFail++;
      this._pollPending = null;
      this._scheduleNextPoll(pending.kind, CFG.POLL_RETRY_MS);
      return;
    }
    const plain = this._session.openResponse(
      rm.ciphertext, resp.nonce, resp.counter, resp.tag, rm.fault, rm.flags, pending.reqTag);
    this._pollPending = null;
    if (!plain) {
      this.stats.respFail++;
      this._logThrottled('响应解密/认证失败（累计 ' + this.stats.respFail + '）', 3000);
      this._scheduleNextPoll(pending.kind, CFG.POLL_RETRY_MS);
      this._emit({ type: 'fault', fault: FAULT_INVALID_SIGNATURE });
      return;
    }
    this.stats.respOk++;
    // 成功响应：drive 按高频周期排，辅助类别按 31s 周期排
    this._scheduleNextPoll(pending.kind,
      pending.kind === 'drive' ? CFG.DRIVE_POLL_INTERVAL_MS : CFG.AUX_POLL_INTERVAL_MS);
    this._applyVehicleData(plain);
  }

  // ── 明文 CarServer.Response → 车辆状态 ───────────────────────────
  _applyVehicleData(plain) {
    const dec = parseCarServerResponse(plain);
    if (!dec || !dec.ok) {
      this.stats.decodeFail++;
      this._logThrottled('CarServer.Response 解码失败（累计 ' + this.stats.decodeFail + '）', 5000);
      return;
    }
    const vd = dec.vehicleData;
    if (!vd) {
      if (dec.actionOk === false) this._log('命令被车端拒绝（actionStatus ERROR）');
      return;
    }
    const upd = {};
    if (vd.drive) {
      if (vd.drive.speedMph !== undefined && isFinite(vd.drive.speedMph)) {
        upd.speedKmh = vd.drive.speedMph * MILES_TO_KM;
      }
      if (vd.drive.powerKw !== undefined) upd.motorPowerKw = vd.drive.powerKw;
      if (vd.drive.gear) upd.gear = vd.drive.gear;
    }
    if (vd.charge) {
      if (vd.charge.soc !== undefined) upd.batteryLevel = vd.charge.soc;
      if (vd.charge.rangeMiles !== undefined && isFinite(vd.charge.rangeMiles)) {
        upd.batteryRangeKm = vd.charge.rangeMiles * MILES_TO_KM;
      }
      if (vd.charge.charging !== undefined) upd.charging = vd.charge.charging;
      if (vd.charge.chargePowerKw !== undefined) upd.chargePowerKw = vd.charge.chargePowerKw;
    }
    if (vd.climate) {
      if (vd.climate.insideTemp !== undefined) upd.insideTemp = vd.climate.insideTemp;
      if (vd.climate.outsideTemp !== undefined) upd.outsideTemp = vd.climate.outsideTemp;
    }
    if (vd.closures) {
      if (vd.closures.doors) upd.doors = vd.closures.doors;
      if (vd.closures.locked !== undefined) upd.locked = vd.closures.locked;
    }
    if (vd.tirePressure && vd.tirePressure.tpms) upd.tpms = vd.tirePressure.tpms;
    this._vstate.applyPartial(upd);
    this._emit({ type: 'data' });
  }

  // ── 发送与工具 ────────────────────────────────────────────────────
  // 按类别排下一次轮询时间（drive 与辅助类别各自独立周期）
  _scheduleNextPoll(kind, ms) {
    if (kind === 'drive') this._nextDriveAt = nowMs() + ms;
    else this._nextAuxAt = nowMs() + ms;
  }

  _send(framed) {
    return this._link.send(framed);
  }

  _log(msg) { this._onLog(msg); }

  _logThrottled(msg, intervalMs) {
    const now = nowMs();
    if (now - this._lastFaultLogAt >= intervalMs) {
      this._lastFaultLogAt = now;
      this._log(msg);
    }
  }

  _logFault(fault, prefix) {
    const name = FAULT_NAMES[fault] || ('fault=' + fault);
    if (fault === FAULT_RESPONSE_MTU_EXCEEDED) {
      this._logThrottled(prefix + ' fault=25 响应超过 MTU（检查连接 MTU 或减少请求字段）', 5000);
      return;
    }
    this._log(prefix + ' fault=' + fault + ' (' + name + ')');
    this._emit({ type: 'fault', fault });
  }

  _emit(ev) { this._onEvent(ev); }

  // 调试：客户端公钥 hex
  pubHex() { return this._pub ? toHex(this._pub) : ''; }
}

module.exports = { TeslaClient };
