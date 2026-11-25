// Canvas 2D 仪表绘制：行驶页 / 充电页 / 链路状态页 / 门状态叠加
'use strict';

const T = require('./theme');

// 与 core/uiModel.STALE_TEXT 保持一致的降级占位符
const STALE_TEXT = '--';

// ── 颜色工具 ───────────────────────────────────────────────────────
function hexToRgb(h) {
  const v = parseInt(h.slice(1), 16);
  return [(v >> 16) & 255, (v >> 8) & 255, v & 255];
}

function lerpColor(h1, h2, t) {
  const a = hexToRgb(h1), b = hexToRgb(h2);
  const r = a.map((x, i) => Math.round(x + (b[i] - x) * t));
  return 'rgb(' + r[0] + ',' + r[1] + ',' + r[2] + ')';
}

// 速度环配色插值：0 → cyan, 50 → green, 100 → amber, 150 → red
const SPEED_NODES = [0, 50, 100, 150];
const SPEED_COLORS = [T.GAUGE_CYAN, T.REGEN_GREEN, T.CAUTION, T.TESLA_RED];

function speedColor(v) {
  for (let i = 0; i < SPEED_NODES.length - 1; i++) {
    if (v <= SPEED_NODES[i + 1]) {
      const t = (v - SPEED_NODES[i]) / (SPEED_NODES[i + 1] - SPEED_NODES[i]);
      return lerpColor(SPEED_COLORS[i], SPEED_COLORS[i + 1], Math.max(0, Math.min(1, t)));
    }
  }
  return T.TESLA_RED;
}

function withAlpha(color, a) {
  const rgb = color[0] === '#' ? hexToRgb(color) : color.match(/\d+/g).map(Number);
  return 'rgba(' + rgb[0] + ',' + rgb[1] + ',' + rgb[2] + ',' + a + ')';
}

// ── 圆环 ───────────────────────────────────────────────────────────
const RING_START = Math.PI * 0.75;  // 左下
const RING_SWEEP = Math.PI * 1.5;   // 270°

function drawTrack(ctx, cx, cy, R, width) {
  ctx.beginPath();
  ctx.arc(cx, cy, R, RING_START, RING_START + RING_SWEEP);
  ctx.strokeStyle = T.TRACK;
  ctx.lineWidth = width;
  ctx.lineCap = 'round';
  ctx.stroke();
}

function drawValueRing(ctx, cx, cy, R, width, frac, color) {
  if (frac <= 0) return;
  const end = RING_START + RING_SWEEP * Math.min(1, frac);
  ctx.beginPath();
  ctx.arc(cx, cy, R, RING_START, end);
  ctx.strokeStyle = color;
  ctx.lineWidth = width;
  ctx.lineCap = 'round';
  ctx.stroke();
}

function drawSegmentedRing(ctx, cx, cy, R, width, val, max) {
  drawTrack(ctx, cx, cy, R, width);
  if (val === null || val === undefined) return;
  const frac = Math.max(0, Math.min(1, val / max));
  const segs = 90;
  for (let i = 0; i < segs; i++) {
    const f = i / segs;
    if (f > frac) break;
    const a0 = RING_START + RING_SWEEP * f;
    const a1 = RING_START + RING_SWEEP * ((i + 1) / segs) + 0.004;
    ctx.beginPath();
    ctx.arc(cx, cy, R, a0, a1);
    ctx.strokeStyle = speedColor(f * max);
    ctx.lineWidth = width;
    ctx.lineCap = 'butt';
    ctx.stroke();
  }
}

// ── 文本 ───────────────────────────────────────────────────────────
function text(ctx, str, x, y, size, color, opts) {
  opts = opts || {};
  ctx.font = (opts.bold ? '600 ' : '') + size + 'px ' + T.FONT;
  ctx.fillStyle = color;
  ctx.textAlign = opts.align || 'center';
  ctx.textBaseline = opts.baseline || 'middle';
  ctx.fillText(str, x, y);
}

// ── 行驶页 ─────────────────────────────────────────────────────────
function drawDrive(ctx, W, H, m, tick) {
  const u = W / 400;
  const cx = W / 2;
  const cy = H * 0.44;
  const R = Math.min(W, H) * 0.33;
  const ringW = R * 0.13;

  // 顶栏
  text(ctx, 'TESLA', W * 0.08, H * 0.05, 22 * u, T.FAINT, { bold: true, align: 'left' });
  if (m.locked === true) text(ctx, 'LOCKED', W * 0.92, H * 0.05, 18 * u, T.REGEN_GREEN, { align: 'right', bold: true });
  else if (m.locked === false) text(ctx, 'UNLOCKED', W * 0.92, H * 0.05, 18 * u, T.ALERT, { align: 'right', bold: true });
  else text(ctx, '--', W * 0.92, H * 0.05, 18 * u, T.FAINT, { align: 'right' });

  // 速度环
  drawSegmentedRing(ctx, cx, cy, R, ringW, m.drive.speedVal, 150);
  text(ctx, m.drive.speedText, cx, cy - 18 * u, 96 * u, T.TEXT, { bold: true });
  text(ctx, 'km/h', cx, cy + 46 * u, 22 * u, T.DIM);
  // 档位
  text(ctx, 'GEAR ' + m.drive.gearText, cx, cy + R + 34 * u, 24 * u, T.GAUGE_CYAN, { bold: true });

  // 功率条（回收=绿 / 输出=红）
  const barY = H * 0.78;
  const barW = W * 0.56;
  const bx = cx - barW / 2;
  ctx.beginPath();
  ctx.arc(bx, barY, 5 * u, Math.PI / 2, Math.PI * 1.5);
  ctx.arc(bx + barW, barY, 5 * u, -Math.PI / 2, Math.PI / 2);
  ctx.closePath();
  ctx.fillStyle = T.TRACK;
  ctx.fill();
  const p = m.drive.powerKw;
  if (p !== null && !isNaN(p)) {
    const FULL = 100;
    const clamped = Math.max(-FULL, Math.min(FULL, p));
    const half = barW / 2;
    const w = (Math.abs(clamped) / FULL) * half;
    ctx.fillStyle = clamped >= 0 ? T.TESLA_RED : T.REGEN_GREEN;
    const sx = clamped >= 0 ? bx + half : bx + half - w;
    roundRect(ctx, sx, barY - 4 * u, w, 8 * u, 4 * u);
    ctx.fill();
    text(ctx, clamped >= 0 ? 'POWER ' + m.drive.powerText + ' kW' : 'REGEN ' + (-clamped).toFixed(1) + ' kW',
      cx, barY - 20 * u, 20 * u, clamped >= 0 ? T.TESLA_RED : T.REGEN_GREEN, { bold: true });
  } else {
    text(ctx, 'POWER ' + m.drive.powerText + ' kW', cx, barY - 20 * u, 20 * u, T.DIM);
  }

  // 电量 / 续航
  text(ctx, 'SOC ' + m.drive.socText + '%  ·  ' + m.drive.rangeText + ' km',
    cx, barY + 36 * u, 24 * u, T.GAUGE_CYAN, { bold: true });

  // 温度 + 胎压
  const footY = H * 0.94;
  text(ctx, 'IN ' + m.climate.insideText + '°C   OUT ' + m.climate.outsideText + '°C',
    W * 0.3, footY, 20 * u, T.DIM);
  const tp = m.tpms.values;
  const lowAny = tp.some((t) => t.low);
  // 胎压过低：红色闪烁告警
  const tpColor = lowAny ? (((tick / 400) | 0) % 2 === 0 ? T.ALERT : withAlpha(T.ALERT, 0.35)) : T.DIM;
  const tpText = tp.map((t) => t.text).join(' / ');
  text(ctx, 'TPMS ' + tpText + ' bar', W * 0.72, footY, 20 * u, tpColor, { align: 'center' });

  // 门状态俯视图（常驻：数据未到时显示 DOORS --，开门时红色闪烁）
  drawDoorsOverlay(ctx, W, H, m, tick);
}

// ── 充电页 ─────────────────────────────────────────────────────────
function drawCharge(ctx, W, H, m) {
  const u = W / 400;
  const cx = W / 2;
  const cy = H * 0.44;
  const R = Math.min(W, H) * 0.33;
  const ringW = R * 0.13;

  text(ctx, 'CHARGING', W * 0.08, H * 0.05, 22 * u, T.REGEN_GREEN, { bold: true, align: 'left' });

  drawTrack(ctx, cx, cy, R, ringW);
  if (m.charge.socVal !== null) {
    drawValueRing(ctx, cx, cy, R, ringW, m.charge.socVal / 100, T.REGEN_GREEN);
  }
  text(ctx, m.charge.socText, cx, cy - 18 * u, 96 * u, T.TEXT, { bold: true });
  text(ctx, '% SOC', cx, cy + 46 * u, 22 * u, T.DIM);

  text(ctx, 'POWER ' + m.charge.powerText + ' kW', cx, cy + R + 34 * u, 26 * u, T.REGEN_GREEN, { bold: true });
  text(ctx, 'RANGE ' + m.charge.rangeText + ' km', cx, cy + R + 70 * u, 22 * u, T.DIM);
  // 充电完成时间估算
  text(ctx, 'ETA ' + m.charge.etaText, cx, cy + R + 104 * u, 22 * u,
    m.charge.etaText === STALE_TEXT ? T.FAINT : T.GAUGE_CYAN, { bold: true });

  const footY = H * 0.94;
  text(ctx, 'IN ' + m.climate.insideText + '°C   OUT ' + m.climate.outsideText + '°C',
    cx, footY, 20 * u, T.DIM);
}

// ── 链路状态页 ─────────────────────────────────────────────────────
function drawLink(ctx, W, H, m, tick) {
  const u = W / 400;
  const cx = W / 2;
  const cy = H * 0.40;
  const R = Math.min(W, H) * 0.30;

  // 呼吸圆环
  const phase = (tick / 1000) % 1;
  ctx.beginPath();
  ctx.arc(cx, cy, R, RING_START, RING_START + RING_SWEEP * phase);
  const isError = m.linkState === 'error';
  const isReadyish = m.linkState === 'ready';
  ctx.strokeStyle = isError ? T.ALERT : (isReadyish ? T.REGEN_GREEN : T.CAUTION);
  ctx.lineWidth = 8 * u;
  ctx.lineCap = 'round';
  ctx.stroke();
  drawTrack(ctx, cx, cy, R, 3 * u);

  text(ctx, m.linkState.toUpperCase(), cx, cy, 40 * u, T.TEXT, { bold: true });
  text(ctx, m.linkDetail || '', cx, cy + R + 44 * u, 22 * u, T.DIM);
  text(ctx, 'TESLA BLE DASHBOARD', cx, H * 0.08, 22 * u, T.FAINT, { bold: true });
  text(ctx, '无数据降级显示 —— 握手成功前不展示任何业务数据', cx, H * 0.9, 18 * u, T.FAINT);
}

// ── 门状态俯视图（行驶页右上角常驻：6 开口状态，开门红色闪烁）──────
function drawDoorsOverlay(ctx, W, H, m, tick) {
  const u = W / 400;
  const carW = W * 0.30;
  const carH = carW * 1.9;
  const x = W - carW - 16 * u;
  const y = H * 0.10;
  const blink = ((tick / 400) | 0) % 2 === 0;

  roundRect(ctx, x, y, carW, carH, 14 * u);
  ctx.strokeStyle = T.DIM;
  ctx.lineWidth = 2 * u;
  ctx.stroke();

  if (m.doors.unknown) {
    // 门数据未到：只画轮廓 + 降级文本
    text(ctx, 'DOORS --', x + carW / 2, y + carH / 2, 14 * u, T.FAINT);
    return;
  }

  const d = m.doors.doors;
  // [fl, fr, rl, rr, frunk, trunk]
  const marks = [
    { open: d[0], x: x - 12 * u, y: y + carH * 0.22, w: 12 * u, h: carH * 0.2, label: 'FL' },
    { open: d[1], x: x + carW, y: y + carH * 0.22, w: 12 * u, h: carH * 0.2, label: 'FR' },
    { open: d[2], x: x - 12 * u, y: y + carH * 0.55, w: 12 * u, h: carH * 0.2, label: 'RL' },
    { open: d[3], x: x + carW, y: y + carH * 0.55, w: 12 * u, h: carH * 0.2, label: 'RR' },
    { open: d[4], x: x, y: y - 8 * u, w: carW, h: 8 * u, label: 'F' },
    { open: d[5], x: x, y: y + carH, w: carW, h: 8 * u, label: 'T' },
  ];
  for (const mk of marks) {
    // 开门：红色闪烁；关闭：暗色常显
    ctx.fillStyle = mk.open ? (blink ? T.ALERT : withAlpha(T.ALERT, 0.4)) : withAlpha(T.DIM, 0.55);
    ctx.fillRect(mk.x, mk.y, mk.w, mk.h);
    if (mk.open) {
      text(ctx, mk.label, mk.x + mk.w / 2, mk.y + mk.h / 2, 11 * u,
        blink ? T.SCREEN : withAlpha(T.SCREEN, 0.6), { bold: true });
    }
  }
}

function roundRect(ctx, x, y, w, h, r) {
  ctx.beginPath();
  ctx.moveTo(x + r, y);
  ctx.arcTo(x + w, y, x + w, y + h, r);
  ctx.arcTo(x + w, y + h, x, y + h, r);
  ctx.arcTo(x, y + h, x, y, r);
  ctx.arcTo(x, y, x + w, y, r);
  ctx.closePath();
}

// ── 总入口 ─────────────────────────────────────────────────────────
function drawFrame(ctx, W, H, model, tick) {
  ctx.fillStyle = T.SCREEN;
  ctx.fillRect(0, 0, W, H);
  if (model.page === 'drive') drawDrive(ctx, W, H, model, tick);
  else if (model.page === 'charge') drawCharge(ctx, W, H, model);
  else drawLink(ctx, W, H, model, tick);
}

module.exports = { drawFrame, speedColor };
