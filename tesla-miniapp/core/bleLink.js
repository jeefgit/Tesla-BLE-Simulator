// BLE 链路层：扫描（广播名精确匹配）→ 连接 → 服务/特征发现 → 订阅 Notify
// 发送封装：writeType='write' 强制带响应 + MTU-3 分片；断线自动重连
'use strict';

const { UUID, CFG } = require('../config/index');
const { advertisedNamesFromVin } = require('./advertisedName');
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

// 广播名比较：忽略大小写和空格（不同平台可能把 "Tesla 571213" 读成 "Tesla571213"）
function nameNorm(n) {
  return String(n || '').replace(/\s+/g, '').toUpperCase();
}

// 真车旧命名最后一位可能是 C/R/D/P，按前 17 位匹配
function isLegacyName(n) {
  return /^S[0-9A-F]{16}[CRDP]$/.test(n);
}

function nameMatches(name, targets) {
  const n = nameNorm(name);
  if (!n) return false;
  for (let i = 0; i < targets.length; i++) {
    const t = targets[i];
    if (n === t) return true;
    if (isLegacyName(n) && isLegacyName(t) && n.slice(0, 17) === t.slice(0, 17)) return true;
  }
  return false;
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
    this._scanCancel = null;
    this._manualDeviceId = null; // 用户手动选择的设备，断线重连时优先直连
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

  // 当前 VIN 对应的候选广播名（VIN 推导名 + 可选的 BLE_NAME 手动名）
  _targetNamesFor(vin) {
    const names = advertisedNamesFromVin(vin);
    const extra = String(CFG.BLE_NAME || '').trim();
    if (extra) {
      const n = nameNorm(extra);
      if (!names.some((x) => nameNorm(x) === n)) names.unshift(extra);
    }
    return names;
  }

  // 供界面展示“正在找哪些广播名”
  targetNamesFor(vin) {
    return this._targetNamesFor(vin);
  }

  // 扫描所有 BLE 设备供界面手动选择（不按名称过滤）。
  // onUpdate(list) 会在扫描过程中反复回调，list 项：
  // { deviceId, name, localName, displayName, RSSI, suggested }
  async scanDevices(vin, timeoutMs, onUpdate) {
    await this._ensureAdapter();
    const targets = this._targetNamesFor(vin);
    const found = new Map();
    let lastEmit = 0;

    const sortList = () => Array.from(found.values()).sort(
      (a, b) => (b.suggested ? 1 : 0) - (a.suggested ? 1 : 0) || b.RSSI - a.RSSI);

    const emit = (force) => {
      if (!onUpdate) return;
      const now = Date.now();
      if (!force && now - lastEmit < 400) return;
      lastEmit = now;
      onUpdate(sortList());
    };

    const collect = (devices) => {
      let changed = false;
      for (let i = 0; i < devices.length; i++) {
        const d = devices[i];
        if (!d || !d.deviceId) continue;
        if (d.connectable === false) continue;
        const prev = found.get(d.deviceId);
        // 后续广播可能只带其中一项，合并而不是覆盖，避免名字闪一下又消失
        const name = d.name || (prev && prev.name) || '';
        const localName = d.localName || (prev && prev.localName) || '';
        if (!name && !localName) continue; // 未命名设备无法在界面上识别
        const suggested = nameMatches(name, targets) || nameMatches(localName, targets);
        found.set(d.deviceId, {
          deviceId: d.deviceId,
          name: name,
          localName: localName,
          displayName: localName || name,
          RSSI: typeof d.RSSI === 'number' ? d.RSSI : -127,
          suggested: suggested || !!(prev && prev.suggested),
        });
        changed = true;
      }
      if (changed) emit(false);
    };

    this._log('[scan] 扫描所有 BLE 设备（目标名 ' + targets.join(' / ') + '）');
    return new Promise((resolve, reject) => {
      let done = false;
      const finish = (fn, arg) => {
        if (done) return;
        done = true;
        clearTimeout(timer);
        this._scanCancel = null;
        wx.offBluetoothDeviceFound();
        this._stopScan();
        fn(arg);
      };
      const timer = setTimeout(() => {
        emit(true);
        finish(resolve, sortList());
      }, timeoutMs);

      this._scanCancel = () => finish(resolve, sortList());

      wx.onBluetoothDeviceFound((res) => {
        collect((res && res.devices) || []);
      });

      wxp(wx.startBluetoothDevicesDiscovery, { allowDuplicatesKey: true })
        .then(() => wxp(wx.getBluetoothDevices).catch(() => null))
        .then((res) => {
          if (res) collect(res.devices || []);
        })
        .catch((e) => finish(reject, e));
    });
  }

  // 界面取消/选中设备时立即结束扫描
  stopScanNow() {
    if (this._scanCancel) this._scanCancel();
    else this._stopScan();
  }

  async _scan(targetNames, timeoutMs) {
    const targets = (Array.isArray(targetNames) ? targetNames : [targetNames])
      .map(nameNorm).filter(Boolean);
    const seen = Object.create(null);
    this._log('[scan] 目标广播名: ' + targets.join(' 或 '));

    const findMatch = (devices) => {
      for (let i = 0; i < devices.length; i++) {
        const d = devices[i];
        // 微信的 name 可能是缓存/车机自定义名，localName 才是广播名，两者都要检查
        const name = d.name || '';
        const localName = d.localName || '';
        const k1 = nameNorm(name);
        const k2 = nameNorm(localName);
        if (k1 && !seen[k1]) seen[k1] = true;
        if (k2 && !seen[k2]) seen[k2] = true;
        if (nameMatches(name, targets) || nameMatches(localName, targets)) return d;
      }
      return null;
    };

    return new Promise((resolve, reject) => {
      let done = false;
      const finish = (fn, arg) => {
        if (done) return;
        done = true;
        clearTimeout(timer);
        wx.offBluetoothDeviceFound();
        this._stopScan();
        fn(arg);
      };
      const timer = setTimeout(() => {
        const nearby = Object.keys(seen).filter(Boolean).slice(0, 15);
        let hint = nearby.length
          ? '；附近设备广播名: ' + nearby.join(', ')
          : '；未收到任何带名称的 BLE 广播（检查手机蓝牙/定位权限，或车辆是否在范围内）';
        // 收到疑似特斯拉广播名但与 VIN 推导名不一致 → 基本可以判定 VIN 配置不是这辆车
        const teslaLike = nearby.filter((n) => /^TESLA/.test(n) || /^S[0-9A-F]{17}$/.test(n));
        if (teslaLike.length) {
          hint += '；检测到疑似特斯拉广播名 ' + teslaLike.join(', ')
            + '，但与当前 VIN 推导的 ' + targets.join(' / ')
            + ' 不一致，请确认 VIN 是这辆真车的 VIN';
        }
        finish(reject, new Error('扫描超时（' + timeoutMs / 1000 + 's），未发现 '
          + targets.join(' 或 ') + hint));
      }, timeoutMs);

      // 先注册监听再开扫描，避免漏掉扫描启动瞬间上报的设备
      wx.onBluetoothDeviceFound((res) => {
        const dev = findMatch((res && res.devices) || []);
        if (dev) finish(resolve, dev);
      });

      // 不做 services 过滤：真车通常不在广播包里携带 0211 服务 UUID，
      // 只有模拟器（BLESS/Bumble）会把 0211 放进广播包，过滤会导致真车永远搜不到。
      wxp(wx.startBluetoothDevicesDiscovery, { allowDuplicatesKey: true })
        .then(() => wxp(wx.getBluetoothDevices).catch(() => null))
        .then((res) => {
          if (!res) return;
          const dev = findMatch(res.devices || []);
          if (dev) finish(resolve, dev);
        })
        .catch((e) => finish(reject, e));
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
    vin = String(vin || '').trim().toUpperCase();
    this._vin = vin;
    this._manualDeviceId = null;
    const targetNames = this._targetNamesFor(vin);
    this._setState(STATE.SCANNING, '按广播名查找 ' + targetNames.join(' / '));
    try {
      await this._ensureAdapter();
      const dev = await this._scan(targetNames, CFG.CONNECT_TIMEOUT_MS);
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

  // 直接连接界面上选中的设备（跳过按广播名扫描）。
  // 真车广播名可能被系统替换成车机自定义名称，按名匹配不可靠时用这条路径。
  async connectDevice(deviceId, vin) {
    if (!deviceId) throw new Error('未选择设备');
    if (this._state === STATE.READY || this._state === STATE.CONNECTING) {
      throw new Error('当前已在连接流程中');
    }
    this.stopScanNow(); // 结束可能仍在进行的扫描
    // 部分 Android ROM 在 stopBluetoothDevicesDiscovery 尚未完成时发起连接会报错，
    // 留一点时间让扫描真正停止。
    await new Promise((resolve) => setTimeout(resolve, 200));
    this._wantDisconnect = false;
    const gen = ++this._gen;
    clearTimeout(this._reconnectTimer);
    this._vin = String(vin || '').trim().toUpperCase();
    this._manualDeviceId = deviceId;
    this._deviceId = deviceId;
    this._setState(STATE.CONNECTING, 'deviceId=' + deviceId);
    try {
      await this._ensureAdapter();
      if (gen !== this._gen) return;
      await wxp(wx.createBLEConnection, { deviceId: deviceId, timeout: 8000 });
      if (gen !== this._gen) return;
      await this._discover();
      await this._negotiateMtu();
      await this._subscribe();
      if (gen !== this._gen) return;
      this._setState(STATE.READY, '手动选择 分片=' + this._chunkSize + 'B');
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
    // 手动选择过设备时优先直连（真车广播名可能不是 VIN 推导名，按名扫描会失败）
    if (this._manualDeviceId) {
      this._setState(STATE.RECONNECTING, '重连上次选择的设备');
      try {
        await this._ensureAdapter();
        if (gen !== this._gen) return;
        this._deviceId = this._manualDeviceId;
        await wxp(wx.createBLEConnection, { deviceId: this._manualDeviceId, timeout: 8000 });
        if (gen !== this._gen) return;
        await this._discover();
        await this._negotiateMtu();
        await this._subscribe();
        if (gen !== this._gen) return;
        this._setState(STATE.READY, '重连成功 分片=' + this._chunkSize + 'B');
        return;
      } catch (e) {
        if (gen !== this._gen) return;
        this._log('[reconnect] 直连失败，改为按广播名扫描: ' + e.message);
        this._deviceId = null;
      }
    }
    const targetNames = this._targetNamesFor(this._vin);
    this._setState(STATE.RECONNECTING, '重试扫描 ' + targetNames.join(' / '));
    await this._ensureAdapter();
    const dev = await this._scan(targetNames, CFG.CONNECT_TIMEOUT_MS);
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
    this._manualDeviceId = null;
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
