// M4 主仪表页：BLE → TeslaClient（握手 + 加密轮询 + plain 回退）→ 车辆快照 → Canvas 绘制
'use strict';

const config = require('../../config/index');
const { BleLink } = require('../../core/bleLink');
const { VehicleState } = require('../../core/vehicleState');
const { TeslaClient } = require('../../core/teslaClient');
const uiModel = require('../../core/uiModel');
const gauge = require('../../ui/gauge');

const REDRAW_MS = 100;
const MAX_LOG = 40;

function loadPrivKeyHex() {
  try {
    const secret = require('../../config/secret.js');
    return secret.CLIENT_KEY_HEX || '';
  } catch (e) {
    return '';
  }
}

Page({
  data: {
    vin: '',
    showDebug: false,
    stateText: 'idle',
    statsText: '',
    logs: [],
    // 配钥浮层：phase = sending|waiting|ok|failed
    pair: { active: false, phase: '', title: '', sub: '', countdown: 0, sends: 0, maxSends: 0 },
  },

  onLoad() {
    const vin = wx.getStorageSync('vin') || config.CFG.VIN;
    this.setData({ vin });

    // 数据层
    this._vstate = new VehicleState();
    this._tick = 0;

    // BLE 链路
    this._link = new BleLink();
    this._link.on('state', (s) => {
      this._vstate.setLink(s.state, s.detail);
      this.setData({ stateText: s.state + (s.detail ? ' · ' + s.detail : '') });
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

    // 快照变化仅用于调试计数，绘制走 100ms 轮询
    this._vstate.subscribe(() => {
      const st = this._link.stats;
      this.setData({ statsText: 'rx ' + st.rxBytes + 'B/' + st.rxFrames + '帧' });
    });
  },

  onReady() {
    this.setupCanvas();
    this._redrawTimer = setInterval(() => this.render(), REDRAW_MS);
  },

  onUnload() {
    clearInterval(this._redrawTimer);
    this._client.stop();
    this._link.disconnect();
  },

  // ── Canvas 初始化 ─────────────────────────────────────────────────
  setupCanvas() {
    const query = wx.createSelectorQuery().in(this);
    query.select('#gauge').fields({ node: true, size: true }).exec((res) => {
      if (!res || !res[0] || !res[0].node) return;
      const canvas = res[0].node;
      let dpr = 2;
      try { dpr = wx.getSystemInfoSync().pixelRatio || 2; } catch (e) { /* 默认 2 */ }
      canvas.width = res[0].width * dpr;
      canvas.height = res[0].height * dpr;
      this._ctx = canvas.getContext('2d');
      this._ctx.scale(dpr, dpr);
      this._cvW = res[0].width;
      this._cvH = res[0].height;
    });
  },

  render() {
    if (!this._ctx) return;
    this._tick += REDRAW_MS;
    const model = uiModel.build(this._vstate.get());
    gauge.drawFrame(this._ctx, this._cvW, this._cvH, model, this._tick);
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
  tapConnect() {
    const vin = this.data.vin;
    if (!/^[A-HJ-NPR-Z0-9]{11,17}$/.test(vin)) {
      this.pushLog('VIN 格式不合法：' + vin);
      return;
    }
    this._link.connect(vin).catch((e) => this.pushLog('连接失败: ' + e.message));
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
