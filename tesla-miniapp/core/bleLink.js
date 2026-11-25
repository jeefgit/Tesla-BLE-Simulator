// BLE 链路层：扫描（广播名精确匹配）→ 连接 → 服务/特征发现 → 订阅 Notify
// 发送封装：writeType='write' 强制带响应 + MTU-3 分片；断线自动重连
'use strict';

const { UUID, CFG } = require('../config/index');
const { advertisedNameFromVin } = require('./advertisedName');
const { toHex } = require('../utils/bytes');

const STATE = {
  IDLE: 'idle',
  SCANNING: 'scanning',
  CONNECTING: 'connecting',
  READY: 'ready',
  RECONNECTING: 'reconnecting',
  ERROR: 'error',
};

// wx API Promise 化
function wxp(fn, opts) {
  return new Promise((resolve, reject) => {
    fn(Object.assign({}, opts || {}, {
      success: resolve,
      fail: (err) => reject(new Error(err && err.errMsg ? err.errMsg : 'wx api fail')),
    }));
  });
}

function uuidNorm(u) {
  return String(u || '').toUpperCase();
}

class BleLink {
  constructor() {
    this._listeners = {};
    this._state = STATE.IDLE;
    this._vin = null;
    this._deviceId = null;
    this._writeCharId = null;
    this._notifyCharId = null;
    this._chunkSize = 20; // ATT 默认 MTU 23 → 20 字节载荷
    this._gen = 0; // 代际计数：断开后旧异步流程全部作废
    this._wantDisconnect = false;
    this._globalHooksInstalled = false;
    this._reconnectTimer = null;
    this._rxBytes = 0;
    this._txBytes = 0;
    this._rxFrames = 0;
    this._txFrames = 0;
  }

  on(event, cb) {
    (this._listeners[event] = this._listeners[event] || []).push(cb);
    return this;
  }

  emit(event, data) {
    const cbs = this._listeners[event] || [];
    for (let i = 0; i < cbs.length; i++) {
      try { cbs[i](data); } catch (e) { /* 监听器异常不影响链路 */ }
    }
  }

  get state() { return this._state; }
  get deviceId() { return this._deviceId; }
  get chunkSize() { return this._chunkSize; }
  get stats() {
    return { rxFrames: this._rxFrames, rxBytes: this._rxBytes, txFrames: this._txFrames, txBytes: this._txBytes };
  }

  _setState(state, detail) {
    this._state = state;
    this.emit('state', { state, detail: detail || '' });
    this.emit('log', '[state] ' + state + (detail ? ' ' + detail : ''));
  }

  _log(msg) {
    this.emit('log', msg);
  }

  // ── 适配器与全局监听 ──────────────────────────────────────────────
  async _ensureAdapter() {
    try {
      await wxp(wx.openBluetoothAdapter);
    } catch (e) {
      throw new Error('蓝牙适配器打开失败（检查手机蓝牙是否已开启）: ' + e.message);
    }
    if (!this._globalHooksInstalled) {
      this._globalHooksInstalled = true;
      wx.onBLEConnectionStateChange((res) => this._onConnState(res));
      wx.onBLECharacteristicValueChange((res) => this._onNotify(res));
    }
  }

  _onConnState(res) {
    if (res.deviceId !== this._deviceId) return;
    if (res.connected) return;
    if (this._wantDisconnect) return;
    this._gen++; // 作废在途异步流程
    this._deviceId = null;
    this._setState(STATE.RECONNECTING, '连接意外断开');
    this._scheduleReconnect();
  }

  _onNotify(res) {
    if (!res.value) return;
    const bytes = new Uint8Array(res.value);
    this._rxBytes += bytes.length;
    this._rxFrames += 1;
    this.emit('bytes', bytes);
  }

  // ── 扫描 ──────────────────────────────────────────────────────────
  _stopScan() {
    wxp(wx.stopBluetoothDevicesDiscovery).catch(() => {});
  }

  async _scan(targetName, timeoutMs) {
    try {
      // services 过滤加速发现（iOS 上可能被忽略，仍需按名称匹配）
      await wxp(wx.startBluetoothDevicesDiscovery, {
        services: [UUID.SERVICE],
        allowDuplicatesKey: false,
      });
    } catch (e) {
      // 某些平台 services 过滤失败时降级为全量扫描
      await wxp(wx.startBluetoothDevicesDiscovery, { allowDuplicatesKey: false });
    }
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        wx.offBluetoothDeviceFound();
        this._stopScan();
        reject(new Error('扫描超时（' + timeoutMs / 1000 + 's），未发现 ' + targetName));
      }, timeoutMs);
      wx.onBluetoothDeviceFound((res) => {
        const devices = (res && res.devices) || [];
        for (let i = 0; i < devices.length; i++) {
          const d = devices[i];
          const name = d.name || d.localName || '';
          if (name === targetName) {
            clearTimeout(timer);
            wx.offBluetoothDeviceFound();
            this._stopScan();
            resolve(d);
            return;
          }
        }
      });
    });
  }

  // ── 服务 / 特征发现 ───────────────────────────────────────────────
  async _discover() {
    const svcsRes = await wxp(wx.getBLEDeviceServices, { deviceId: this._deviceId });
    const services = svcsRes.services || [];
    const svc = services.find((s) => uuidNorm(s.uuid) === uuidNorm(UUID.SERVICE)) ||
      services.find((s) => uuidNorm(s.uuid).indexOf('0211') >= 0);
    if (!svc) throw new Error('未找到 Tesla 服务 0211（发现服务数=' + services.length + '）');
    this._serviceId = svc.uuid;

    const charsRes = await wxp(wx.getBLEDeviceCharacteristics, {
      deviceId: this._deviceId,
      serviceId: this._serviceId,
    });
    const chars = charsRes.characteristics || [];
    const writeChar = chars.find((c) => uuidNorm(c.uuid).indexOf('0212') >= 0 && c.properties && c.properties.write);
    const notifyChar = chars.find((c) => uuidNorm(c.uuid).indexOf('0213') >= 0 && c.properties && (c.properties.notify || c.properties.indicate));
    if (!writeChar) throw new Error('未找到可写特征 0212');
    if (!notifyChar) throw new Error('未找到通知特征 0213');
    this._writeCharId = writeChar.uuid;
    this._notifyCharId = notifyChar.uuid;
    this._log('[gatt] write=' + writeChar.uuid + ' notify=' + notifyChar.uuid);
  }

  // ── MTU ───────────────────────────────────────────────────────────
  async _negotiateMtu() {
    try {
      await wxp(wx.setBLEMTU, { deviceId: this._deviceId, mtu: CFG.MTU_TARGET });
      this._chunkSize = CFG.MTU_TARGET - 3;
      this._log('[mtu] 协商成功 mtu=' + CFG.MTU_TARGET + ' 分片=' + this._chunkSize + 'B');
    } catch (e) {
      // iOS 系统自动协商不可设置；回退 20B 分片（对任何 MTU 都安全）
      this._chunkSize = 20;
      this._log('[mtu] setBLEMTU 失败，回退 20B 分片: ' + e.message);
    }
  }

  // ── 订阅 ──────────────────────────────────────────────────────────
  async _subscribe() {
    await wxp(wx.notifyBLECharacteristicValueChange, {
      deviceId: this._deviceId,
      serviceId: this._serviceId,
      characteristicId: this._notifyCharId,
      state: true,
    });
    this._log('[gatt] notify 已订阅');
  }

  // ── 连接主流程 ────────────────────────────────────────────────────
  async connect(vin) {
    if (this._state === STATE.READY || this._state === STATE.CONNECTING) {
      throw new Error('当前已在连接流程中');
    }
    this._wantDisconnect = false;
    const gen = ++this._gen;
    clearTimeout(this._reconnectTimer); // 取消挂起的重连，避免并发两条连接流程
    this._vin = vin;
    const targetName = advertisedNameFromVin(vin);
    this._setState(STATE.SCANNING, '按广播名查找 ' + targetName);
    try {
      await this._ensureAdapter();
      const dev = await this._scan(targetName, CFG.CONNECT_TIMEOUT_MS);
      if (gen !== this._gen) return;
      this._deviceId = dev.deviceId;
      this._setState(STATE.CONNECTING, 'deviceId=' + dev.deviceId);
      await wxp(wx.createBLEConnection, { deviceId: dev.deviceId, timeout: 8000 });
      if (gen !== this._gen) return;
      await this._discover();
      await this._negotiateMtu();
      await this._subscribe();
      if (gen !== this._gen) return;
      this._setState(STATE.READY, '分片=' + this._chunkSize + 'B');
    } catch (e) {
      if (gen !== this._gen) return;
      this._cleanupDevice();
      this._setState(STATE.ERROR, e.message);
      throw e;
    }
  }

  _cleanupDevice() {
    if (this._deviceId) {
      const id = this._deviceId;
      this._deviceId = null;
      wxp(wx.closeBLEConnection, { deviceId: id }).catch(() => {});
    }
    clearTimeout(this._reconnectTimer);
  }

  // ── 自动重连 ──────────────────────────────────────────────────────
  _scheduleReconnect() {
    clearTimeout(this._reconnectTimer);
    this._reconnectTimer = setTimeout(() => {
      if (this._wantDisconnect) return;
      this._reconnectOnce().catch((e) => {
        this._log('[reconnect] 失败: ' + e.message);
        if (!this._wantDisconnect) this._scheduleReconnect();
      });
    }, CFG.RECONNECT_INTERVAL_MS);
  }

  async _reconnectOnce() {
    const gen = ++this._gen;
    const targetName = advertisedNameFromVin(this._vin);
    this._setState(STATE.RECONNECTING, '重试扫描 ' + targetName);
    await this._ensureAdapter();
    const dev = await this._scan(targetName, CFG.CONNECT_TIMEOUT_MS);
    if (gen !== this._gen) return;
    this._deviceId = dev.deviceId;
    await wxp(wx.createBLEConnection, { deviceId: dev.deviceId, timeout: 8000 });
    if (gen !== this._gen) return;
    await this._discover();
    await this._negotiateMtu();
    await this._subscribe();
    if (gen !== this._gen) return;
    this._setState(STATE.READY, '重连成功 分片=' + this._chunkSize + 'B');
  }

  // ── 发送（带响应写 + MTU-3 分片）─────────────────────────────────
  async send(bytes) {
    if (this._state !== STATE.READY || !this._deviceId) {
      throw new Error('链路未就绪，无法发送');
    }
    let payload = bytes;
    if (payload instanceof ArrayBuffer) payload = new Uint8Array(payload);
    if (!payload || payload.length === 0) throw new Error('拒绝发送空帧');
    const chunks = Math.max(1, Math.ceil(payload.length / this._chunkSize));
    for (let off = 0, idx = 0; off < payload.length; off += this._chunkSize, idx++) {
      const part = payload.subarray(off, Math.min(off + this._chunkSize, payload.length));
      await this._writeOnce(part, idx, chunks);
    }
    this._txBytes += payload.length;
    this._txFrames += 1;
    this.emit('tx', payload);
  }

  async _writeOnce(part, idx, chunks) {
    const value = part.buffer.slice(part.byteOffset, part.byteOffset + part.byteLength);
    const base = {
      deviceId: this._deviceId,
      serviceId: this._serviceId,
      characteristicId: this._writeCharId,
      value,
    };
    try {
      // 硬约束：必须 Write With Response
      await wxp(wx.writeBLECharacteristicValue, Object.assign({}, base, { writeType: 'write' }));
    } catch (e) {
      // 个别 ROM 对 writeType 兼容差：降级一次（默认行为多数平台仍为带响应写）
      this._log('[write] writeType=write 失败(分片 ' + (idx + 1) + '/' + chunks + ')，降级重试: ' + e.message);
      await wxp(wx.writeBLECharacteristicValue, base);
    }
  }

  // ── 断开 ──────────────────────────────────────────────────────────
  async disconnect() {
    this._wantDisconnect = true;
    this._gen++;
    clearTimeout(this._reconnectTimer);
    const id = this._deviceId;
    this._deviceId = null;
    if (id) {
      try { await wxp(wx.closeBLEConnection, { deviceId: id }); } catch (e) { /* 忽略 */ }
    }
    try { await wxp(wx.closeBluetoothAdapter); } catch (e) { /* 忽略 */ }
    this._setState(STATE.IDLE, '已断开');
  }

  // 调试用：最近一帧十六进制
  static hexPreview(bytes, max) {
    const h = toHex(bytes);
    return h.length > max ? h.slice(0, max) + '…' : h;
  }
}

module.exports = { BleLink, STATE };
