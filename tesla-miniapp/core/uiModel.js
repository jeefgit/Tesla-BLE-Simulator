// UI 模型映射：VehicleState 快照 → 仪表页面模型（含降级 '--' 规则与页面状态机）
'use strict';

const STALE_TEXT = '--';

function fmtNum(v, digits, fresh) {
  if (!fresh || v === null || v === undefined || isNaN(v)) return STALE_TEXT;
  return Number(v).toFixed(digits);
}

function fmtInt(v, fresh) {
  if (!fresh || v === null || v === undefined || isNaN(v)) return STALE_TEXT;
  return String(Math.round(v));
}

// 充电完成时间估算：假设 62kWh 电池组，按当前充电功率折算剩余百分比耗时
// 前提：车端正在充电且功率 > 0，否则降级显示 '--'
const BATTERY_KWH = 62;

function chargeEtaText(snap, fresh) {
  if (!fresh) return STALE_TEXT;
  const power = snap.chargePowerKw;
  const soc = snap.batteryLevel;
  if (!snap.charging || power === null || power === undefined || isNaN(power)
    || power <= 0 || soc === null || soc === undefined || isNaN(soc)) {
    return STALE_TEXT;
  }
  const pctPerHour = (power / BATTERY_KWH) * 100;
  const remainPct = Math.max(0, 100 - soc);
  const mins = Math.round((remainPct / pctPerHour) * 60);
  if (!isFinite(mins) || mins <= 0) return STALE_TEXT;
  if (mins >= 60) return Math.floor(mins / 60) + 'h' + (mins % 60) + 'm';
  return mins + 'min';
}

// 门序：0=左前 1=右前 2=左后 3=右后 4=前备箱 5=后备箱
const DOOR_NAMES = ['FL', 'FR', 'RL', 'RR', 'FRUNK', 'TRUNK'];

function build(vs) {
  const { snap, fresh, link } = vs;
  const ready = link.ready;

  // 页面状态机：未就绪 → link 页；就绪后 charging ? charge : drive
  let page = 'link';
  if (ready) {
    page = fresh.charge && snap.charging ? 'charge' : 'drive';
  }

  const model = {
    page,
    linkState: link.state,
    linkDetail: link.detail,
    ready,
    locked: ready && fresh.doors ? snap.locked : null,
    vin: null,
  };

  // 行驶页
  const driveFresh = ready && fresh.drive;
  const battFresh = ready && fresh.battery;
  const gearFresh = ready && fresh.gear;
  // 功率条（WXML 渲染）：-100~100 kW，输出=红 / 回收=绿
  const powerKw = driveFresh ? snap.motorPowerKw : null;
  const hasPower = powerKw !== null && powerKw !== undefined && !isNaN(powerKw);
  model.drive = {
    speedText: fmtInt(snap.speedKmh, driveFresh),
    speedVal: driveFresh ? Math.max(0, Math.min(150, snap.speedKmh || 0)) : null,
    gearText: gearFresh ? (snap.gear || STALE_TEXT) : STALE_TEXT,
    powerKw: powerKw,
    powerText: fmtNum(powerKw, 1, driveFresh),
    hasPower: hasPower,
    powerPct: hasPower ? Math.round(Math.abs(Math.max(-100, Math.min(100, powerKw)))) : 0,
    powerDir: !hasPower || powerKw === 0 ? null : (powerKw > 0 ? 'out' : 'regen'),
    socText: fmtInt(snap.batteryLevel, battFresh),
    socVal: battFresh ? snap.batteryLevel : null,
    rangeText: fmtInt(snap.batteryRangeKm, battFresh),
  };

  // WXML 内联样式串预生成（避免 VS Code 对 style="{{}}" 的 CSS 误报）
  model.drive.powerOutStyle = 'width: ' + (model.drive.powerDir === 'out' ? model.drive.powerPct : 0) + '%';
  model.drive.powerRegenStyle = 'width: ' + (model.drive.powerDir === 'regen' ? model.drive.powerPct : 0) + '%';

  // 充电页
  model.charge = {
    socText: fmtInt(snap.batteryLevel, battFresh),
    socVal: battFresh ? snap.batteryLevel : null,
    powerText: fmtNum(snap.chargePowerKw, 1, ready && fresh.charge),
    rangeText: fmtInt(snap.batteryRangeKm, battFresh),
    etaText: chargeEtaText(snap, ready && fresh.charge),
    charging: ready && fresh.charge ? snap.charging === true : false,
  };
  model.charge.socBarStyle = 'width: ' + (model.charge.socVal || 0) + '%';

  // 门状态（任一打开时叠加显示）
  const doorsFresh = ready && fresh.doors;
  const doors = [];
  let anyOpen = false;
  for (let i = 0; i < 6; i++) {
    const open = doorsFresh ? snap.doors[i] === true : false;
    if (open) anyOpen = true;
    doors.push({ name: DOOR_NAMES[i], open, unknown: !doorsFresh });
  }
  model.doors = { doors, anyOpen, unknown: !doorsFresh };

  // 胎压（bar，低于 2.0 告警）
  const tpmsFresh = ready && fresh.tpms;
  model.tpms = {
    values: ['FL', 'FR', 'RL', 'RR'].map((n, i) => ({
      name: n,
      text: fmtNum(snap.tpms ? snap.tpms[i] : null, 2, tpmsFresh),
      low: tpmsFresh && snap.tpms[i] !== null && snap.tpms[i] > 0 && snap.tpms[i] < 2.0,
    })),
  };

  // 温度
  model.climate = {
    insideText: fmtNum(snap.insideTemp, 1, ready && fresh.climate),
    outsideText: fmtNum(snap.outsideTemp, 1, ready && fresh.climate),
  };

  return model;
}

module.exports = { build, DOOR_NAMES, STALE_TEXT };
