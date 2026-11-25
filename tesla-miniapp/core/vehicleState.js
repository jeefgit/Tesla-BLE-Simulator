// 车辆状态快照：分组时效（DATA_STALE_MS 过期判定）+ 发布订阅
'use strict';

const { CFG } = require('../config/index');

const GROUPS = ['drive', 'battery', 'gear', 'doors', 'charge', 'climate', 'tpms'];

class VehicleState {
  constructor() {
    this._subs = [];
    this._link = { state: 'idle', detail: '', ready: false };
    this._snap = {
      speedKmh: null, motorPowerKw: null,
      batteryLevel: null, batteryRangeKm: null,
      gear: null,
      doors: [null, null, null, null, null, null],
      locked: null,
      charging: null, chargePowerKw: null,
      insideTemp: null, outsideTemp: null,
      tpms: [null, null, null, null],
    };
    this._ts = {};
  }

  subscribe(cb) {
    this._subs.push(cb);
    return () => {
      const i = this._subs.indexOf(cb);
      if (i >= 0) this._subs.splice(i, 1);
    };
  }

  _notify() {
    for (const cb of this._subs) {
      try { cb(this.get()); } catch (e) { /* 忽略订阅者异常 */ }
    }
  }

  setLink(state, detail) {
    const ready = state === 'ready';
    if (this._link.state === state && this._link.detail === detail) return;
    this._link = { state, detail: detail || '', ready };
    // 断线时全部数据立即视为过期
    if (!ready) this._ts = {};
    this._notify();
  }

  applySimPayload(d) {
    const now = Date.now();
    this._snap.speedKmh = d.speedKmh;
    this._snap.motorPowerKw = d.motorPowerKw;
    this._snap.batteryLevel = d.batteryLevel;
    this._snap.batteryRangeKm = d.batteryRangeKm;
    this._snap.gear = d.gear;
    this._snap.doors = d.doors.slice();
    this._snap.locked = d.locked;
    this._snap.charging = d.charging;
    this._snap.chargePowerKw = d.chargePowerKw;
    this._snap.insideTemp = d.insideTemp;
    this._snap.outsideTemp = d.outsideTemp;
    this._snap.tpms = d.tpms.slice();
    this._ts = {};
    for (const g of GROUPS) this._ts[g] = now;
    this._notify();
  }

  // 加密轮询的部分更新：只覆盖出现的字段（undefined 字段保持原值）
  // 字段 → 分组映射与 uiModel 一致
  applyPartial(d) {
    if (!d) return;
    const now = Date.now();
    const groupMap = {
      speedKmh: 'drive', motorPowerKw: 'drive',
      batteryLevel: 'battery', batteryRangeKm: 'battery',
      gear: 'gear',
      doors: 'doors', locked: 'doors',
      charging: 'charge', chargePowerKw: 'charge',
      insideTemp: 'climate', outsideTemp: 'climate',
      tpms: 'tpms',
    };
    let touched = false;
    for (const key in groupMap) {
      const v = d[key];
      if (v === undefined) continue;
      if (Array.isArray(v)) this._snap[key] = v.slice();
      else this._snap[key] = v;
      this._ts[groupMap[key]] = now;
      touched = true;
    }
    if (touched) this._notify();
  }

  get() {
    const now = Date.now();
    const fresh = {};
    for (const g of GROUPS) fresh[g] = (now - (this._ts[g] || 0)) < CFG.DATA_STALE_MS;
    return { snap: this._snap, fresh, link: this._link };
  }
}

module.exports = { VehicleState, GROUPS };
