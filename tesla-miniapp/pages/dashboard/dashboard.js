// 主仪表页：BLE → TeslaClient（握手 + 加密轮询 + plain 回退）→ 车辆快照 → WXML 仪表
'use strict';

const config = require('../../config/index');
const { BleLink } = require('../../core/bleLink');
const { VehicleState } = require('../../core/vehicleState');
const { TeslaClient } = require('../../core/teslaClient');
const uiModel = require('../../core/uiModel');

const REDRAW_MS = 250;
const MAX_LOG = 40;

function loadPrivKeyHex() {
  try {
    const secret = require('../../config/secret.js');
    return secret.CLIENT_KEY_HEX || '';
  } catch (e) {
    return '';
  }
}

// BLE 设备 → 列表项。WeChat 的 name 可能是缓存/车机自定义名，localName 才是广播名，
// 两个都展示，方便确认真车到底广播了什么。
function formatScanDevice(d) {
  const primary = d.localName || d.name || '';
  const secondary = d.name && d.localName && d.name !== d.localName ? d.name : '';
  return {
    deviceId: d.deviceId,
    title: primary || '(未命名设备)',
    meta: [secondary, d.deviceId].filter(Boolean).join(' · '),
    rssiText: typeof d.RSSI === 'number' && d.RSSI > -127 ? d.RSSI + ' dBm' : '',
    suggested: !!d.suggested,
  };
}

Page({
  data: {
    vin: '',
    showDebug: false,
    clock: '18:00',
    stateText: 'idle',
    statsText: '',
    logs: [],
    // 仪表 UI 模型由 WXML 渲染
    model: null,
    // 配钥浮层：phase = sending|waiting|ok|failed
    pair: { active: false, phase: '', title: '', sub: '', countdown: 0, sends: 0, maxSends: 0 },
    // 设备选择浮层：扫描到的 BLE 设备列表，由用户手动选择要连接的车
    scan: { active: false, devices: [], hint: '', targetsText: '' },
  },

  onLoad() {
    const vin = String(wx.getStorageSync('vin') || config.CFG.VIN || '').trim().toUpperCase();
    this.setData({ vin });
    this.updateClock();
    this._clockTimer = setInterval(() => this.updateClock(), 1000);

    // 数据层
    this._vstate = new VehicleState();

    // BLE 链路
    this._link = new BleLink();
    this._link.on('state', (s) => {
      this._vstate.setLink(s.state, s.detail);
      this.setData({ stateText: s.state + (s.detail ? ' · ' + s.detail : '') });
      if (s.state === 'ready' && this.data.scan.active) this.setData({ 'scan.active': false });
      this.pushLog('→ ' + s.state + ' ' + s.detail);
    });

    // 协议客户端：握手 → 加密轮询（明文旁路自动回退）
    this._client = new TeslaClient({
      link: this._link,
      vstate: this._vstate,
      vin: vin,
      privKeyHex: loadPrivKeyHex(),
      onLog: (m) => this.pushLog(m),
      onEvent: (ev) => this.onClientEvent(ev),
    });
    this._client.start();

    // 快照变化用于更新调试计数；仪表模型由定时器刷新以处理数据过期
    this._vstate.subscribe(() => {
      const st = this._link.stats;
      this.setData({ statsText: 'rx ' + st.rxBytes + 'B/' + st.rxFrames + '帧' });
    });
  },

  onReady() {
    this.render();
    this._redrawTimer = setInterval(() => this.render(), REDRAW_MS);
  },

  onUnload() {
    clearInterval(this._redrawTimer);
    clearInterval(this._clockTimer);
    this._scanSeq = (this._scanSeq || 0) + 1;
    this._link.stopScanNow();
    this._client.stop();
    this._link.disconnect();
  },

  updateClock() {
    const now = new Date();
    const clock = ('0' + now.getHours()).slice(-2) + ':' + ('0' + now.getMinutes()).slice(-2);
    if (clock !== this.data.clock) this.setData({ clock });
  },

  render() {
    const model = uiModel.build(this._vstate.get());
    const json = JSON.stringify(model);
    if (json !== this._lastModelJson) {
      this._lastModelJson = json;
      this.setData({ model });
    }
    this.updatePairCountdown();
  },

  // ── 配钥浮层 ──────────────────────────────────────────────────────
  setPair(patch) {
    this.setData({ pair: Object.assign({}, this.data.pair, patch) });
  },

  updatePairCountdown() {
    if (!this.data.pair.active || this.data.pair.phase !== 'waiting' || !this._pairDeadline) return;
    const left = Math.max(0, Math.ceil((this._pairDeadline - Date.now()) / 1000));
    if (left !== this.data.pair.countdown) this.setData({ 'pair.countdown': left });
  },

  // ── 客户端事件 ────────────────────────────────────────────────────
  onClientEvent(ev) {
    switch (ev.type) {
      case 'session':
        if (ev.ok) {
          this._pairDeadline = 0;
          this.setPair({ active: false });
          this.setData({ stateText: 'ready · 加密会话' });
        } else this.pushLog('会话失败: ' + (ev.reason || ''));
        break;
      case 'notPaired':
        this._pairDeadline = 0;
        this.setPair({ active: true, phase: 'sending', title: '钥匙未配对',
          sub: '正在向车端发送配钥请求…', sends: 0, countdown: 0 });
        this.pushLog('本机公钥未配对，进入配钥流程（在模拟器界面轻点钥匙卡）');
        this.setData({ stateText: 'ready · 未配对' });
        break;
      case 'pairing':
        this.onPairingEvent(ev);
        break;
      case 'plainFallback':
        this.setData({ stateText: 'ready · 旁路模式' });
        break;
      case 'fault':
        // fault 已由客户端日志记录
        break;
      case 'error':
        if (ev.message === 'NO_KEY') this.pushLog('缺少私钥：请复制 config/secret.example.js 为 config/secret.js');
        break;
      default:
        break;
    }
  },

  onPairingEvent(ev) {
    switch (ev.phase) {
      case 'sending':
        this.setPair({ phase: 'sending', title: '钥匙未配对',
          sub: '正在向车端发送配钥请求…', sends: ev.sends, maxSends: ev.maxSends });
        break;
      case 'waiting':
        // 车端已受理，等待贴卡确认（倒计时由 render 驱动）
        this._pairDeadline = Date.now() + (ev.waitMs || 0);
        this.setPair({ phase: 'waiting', title: '请轻点钥匙卡',
          sub: '在车端中控感应区轻点钥匙卡确认配钥', countdown: Math.ceil((ev.waitMs || 0) / 1000) });
        break;
      case 'ok':
        this._pairDeadline = 0;
        this.setPair({ phase: 'ok', title: '配钥成功', sub: '正在重新建立加密会话…' });
        setTimeout(() => { if (this.data.pair.phase === 'ok') this.setPair({ active: false }); }, 2500);
        break;
      case 'failed':
        this._pairDeadline = 0;
        this.setPair({ phase: 'failed', title: '配钥未完成',
          sub: '被车端拒绝（info=' + ev.info + '），稍后自动重试' });
        break;
      case 'roundRestart':
        this._pairDeadline = 0;
        this.setPair({ phase: 'sending', title: '钥匙未配对',
          sub: '重新向车端发送配钥请求…' });
        break;
      default:
        break;
    }
  },

  // ── 连接控制 ──────────────────────────────────────────────────────
  // 右侧车锁卡片：已就绪时断开，否则打开设备选择列表
  tapLock() {
    const model = this.data.model;
    if (model && model.ready) this.tapDisconnect();
    else this.tapConnect();
  },

  normalizedVin() {
    const vin = String(this.data.vin || '').trim().toUpperCase();
    if (vin !== this.data.vin) this.setData({ vin });
    if (!/^[A-HJ-NPR-Z0-9]{11,17}$/.test(vin)) {
      this.pushLog('VIN 格式不合法：' + vin);
      return '';
    }
    return vin;
  },

  tapConnect() {
    const vin = this.normalizedVin();
    if (!vin) return;
    this.startScan();
  },

  // 扫描所有 BLE 设备并列出，由用户手动选择要连接的车
  startScan() {
    const vin = this.normalizedVin();
    if (!vin) return;
    const seq = (this._scanSeq || 0) + 1;
    this._scanSeq = seq;
    this._scanning = true;
    const targets = this._link.targetNamesFor(vin);
    this.setData({
      scan: { active: true, devices: [], hint: '正在扫描附近的 BLE 设备…', targetsText: targets.join(' / ') },
    });
    this._link.scanDevices(vin, 10000, (list) => {
      if (this._scanSeq !== seq) return;
      this.setData({
        'scan.devices': list.map(formatScanDevice),
        'scan.hint': '扫描中… 已发现 ' + list.length + ' 个设备',
      });
    }).then((list) => {
      if (this._scanSeq !== seq) return;
      this._scanning = false;
      const devices = list.map(formatScanDevice);
      let hint;
      if (!devices.length) {
        hint = '没有扫描到带名称的 BLE 设备。请确认手机蓝牙已开启、定位权限已允许，车辆在附近并已唤醒。';
      } else if (devices.some((d) => d.suggested)) {
        hint = '已找到与 VIN 匹配的设备（标记“VIN 匹配”），轻触即可连接。';
      } else {
        hint = '未发现与 VIN 推导名称一致的设备。若列表中有你的车，直接轻触它连接即可。';
      }
      this.setData({ scan: { active: true, devices, hint, targetsText: targets.join(' / ') } });
    }).catch((e) => {
      if (this._scanSeq !== seq) return;
      this._scanning = false;
      this.setData({ 'scan.hint': '扫描失败：' + e.message });
    });
  },

  tapRescan() {
    this._scanSeq = (this._scanSeq || 0) + 1;
    this._link.stopScanNow();
    this.setData({ 'scan.hint': '正在重新扫描…' });
    setTimeout(() => {
      if (this.data.scan.active) this.startScan();
    }, 300);
  },

  tapCancelScan() {
    this._scanSeq = (this._scanSeq || 0) + 1;
    this._scanning = false;
    this._link.stopScanNow();
    this.setData({ 'scan.active': false });
  },

  tapPickDevice(e) {
    const deviceId = e.currentTarget.dataset.id;
    const vin = this.normalizedVin();
    if (!deviceId || !vin) return;
    const dev = (this.data.scan.devices || []).find((d) => d.deviceId === deviceId);
    this._scanSeq = (this._scanSeq || 0) + 1;
    this._scanning = false;
    this._link.stopScanNow();
    this.setData({ 'scan.active': false });
    this.pushLog('手动连接：' + (dev ? dev.title : deviceId));
    this._link.connectDevice(deviceId, vin)
      .catch((err) => this.pushLog('连接失败: ' + err.message));
  },

  tapDisconnect() {
    this._link.disconnect().catch((e) => this.pushLog('断开失败: ' + e.message));
  },

  toggleDebug() {
    this.setData({ showDebug: !this.data.showDebug });
  },

  pushLog(line) {
    const now = new Date();
    const ts = ('0' + now.getHours()).slice(-2) + ':' + ('0' + now.getMinutes()).slice(-2) + ':' + ('0' + now.getSeconds()).slice(-2);
    this._logs = this._logs || [];
    this._logs.push(ts + ' ' + line);
    if (this._logs.length > MAX_LOG) this._logs.splice(0, this._logs.length - MAX_LOG);
    this.setData({ logs: this._logs.slice() });
  },

  // 调试用：客户端公钥
  clientPubHex() {
    return this._client ? this._client.pubHex() : '';
  },
});
