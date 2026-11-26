// Canvas 2D 仪表绘制（微信浅色卡片风）：车速环 / 充电环 / 链路状态页
// 门状态、功率条、胎压等已迁移到 WXML 信息卡片，Canvas 只负责圆环主体
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

// 速度环配色插值：0 → 微信蓝, 50 → 微信绿, 100 → 橙, 150 → 红
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

function roundRect(ctx, x, y, w, h, r) {
  ctx.beginPath();
  ctx.moveTo(x + r, y);
  ctx.arcTo(x + w, y, x + w, y + h, r);
  ctx.arcTo(x + w, y + h, x, y + h, r);
  ctx.arcTo(x, y + h, x, y, r);
  ctx.arcTo(x, y, x + w, y, r);
  ctx.closePath();
}

// ── 行驶页：车速环 + 档位徽章 ──────────────────────────────────────
function drawDrive(ctx, W, H, m) {
  const u = W / 400;
  const cx = W / 2;
  const cy = H * 0.44;
  const R = Math.min(W, H) * 0.36;
  const ringW = R * 0.14;

  drawSegmentedRing(ctx, cx, cy, R, ringW, m.drive.speedVal, 150);
  text(ctx, m.drive.speedText, cx, cy - 14 * u, 88 * u, T.TEXT, { bold: true });
  text(ctx, 'km/h', cx, cy + 46 * u, 22 * u, T.DIM);

  // 档位徽章（微信胶囊样式）
  const label = '档位 ' + m.drive.gearText;
  const size = 24 * u;
  ctx.font = '600 ' + size + 'px ' + T.FONT;
  const w = ctx.measureText(label).width + 32 * u;
  const badgeY = cy + R + 20 * u;
  roundRect(ctx, cx - w / 2, badgeY, w, 44 * u, 22 * u);
  ctx.fillStyle = withAlpha(T.GAUGE_CYAN, 0.12);
  ctx.fill();
  text(ctx, label, cx, badgeY + 22 * u, size, T.GAUGE_CYAN, { bold: true });
}

// ── 充电页：SOC 环（微信绿）+ 充电功率 ─────────────────────────────
function drawCharge(ctx, W, H, m) {
  const u = W / 400;
  const cx = W / 2;
  const cy = H * 0.44;
  const R = Math.min(W, H) * 0.36;
  const ringW = R * 0.14;

  drawTrack(ctx, cx, cy, R, ringW);
  if (m.charge.socVal !== null) {
    drawValueRing(ctx, cx, cy, R, ringW, m.charge.socVal / 100, T.REGEN_GREEN);
  }
  text(ctx, m.charge.socText, cx, cy - 14 * u, 88 * u, T.TEXT, { bold: true });
  text(ctx, '% SOC', cx, cy + 46 * u, 22 * u, T.DIM);

  const pw = '充电功率 ' + m.charge.powerText + ' kW';
  text(ctx, pw, cx, cy + R + 42 * u, 24 * u, T.REGEN_GREEN, { bold: true });
}

// ── 链路状态页：呼吸圆环 + 状态文本 ────────────────────────────────
function drawLink(ctx, W, H, m, tick) {
  const u = W / 400;
  const cx = W / 2;
  const cy = H * 0.42;
  const R = Math.min(W, H) * 0.30;

  const phase = (tick / 1000) % 1;
  ctx.beginPath();
  ctx.arc(cx, cy, R, RING_START, RING_START + RING_SWEEP * phase);
  const isError = m.linkState === 'error';
  const isReadyish = m.linkState === 'ready';
  ctx.strokeStyle = isError ? T.ALERT : (isReadyish ? T.REGEN_GREEN : T.GAUGE_CYAN);
  ctx.lineWidth = 8 * u;
  ctx.lineCap = 'round';
  ctx.stroke();
  drawTrack(ctx, cx, cy, R, 3 * u);

  const stateText = {
    idle: '待连接',
    scanning: '扫描中',
    connecting: '连接中',
    reconnecting: '重连中',
    error: '连接异常',
  }[m.linkState] || m.linkState;
  text(ctx, stateText, cx, cy, 40 * u, T.TEXT, { bold: true });
  text(ctx, m.linkDetail || '', cx, cy + R + 40 * u, 22 * u, T.DIM);
  text(ctx, '特斯拉 BLE 仪表盘', cx, H * 0.10, 22 * u, T.FAINT, { bold: true });
  text(ctx, '握手成功前不展示任何业务数据', cx, H * 0.88, 18 * u, T.FAINT);
}

// ── 驾驶舱页：参考仪表总览布局（车外温度由页面顶栏展示） ───────────────
function drawCockpit(ctx, W, H, m) {
  const u = Math.min(W / 760, H / 360);
  const cx = W * 0.5;
  const cy = H * 0.48;
  const ringR = Math.min(W * 0.205, H * 0.48);

  ctx.fillStyle = '#080D18';
  ctx.fillRect(0, 0, W, H);

  const panel = ctx.createRadialGradient(cx, cy, ringR * 0.1, cx, cy, ringR * 1.8);
  panel.addColorStop(0, '#10253E');
  panel.addColorStop(0.55, '#08111F');
  panel.addColorStop(1, '#080D18');
  ctx.fillStyle = panel;
  ctx.fillRect(0, 0, W, H);

  // 左侧垂直分栏和俯视车
  const leftX = W * 0.09;
  ctx.fillStyle = '#0D1526';
  ctx.fillRect(W * 0.015, H * 0.08, W * 0.19, H * 0.62);
  ctx.strokeStyle = '#26304A';
  ctx.lineWidth = 1 * u;
  ctx.strokeRect(W * 0.205, H * 0.08, W * 0.01, H * 0.62);

  const carW = 92 * u;
  const carH = 250 * u;
  const carY = cy - carH * 0.46;
  ctx.beginPath();
  ctx.moveTo(leftX - carW * 0.28, carY + carH * 0.13);
  ctx.quadraticCurveTo(leftX - carW * 0.34, carY + carH * 0.06, leftX - carW * 0.18, carY);
  ctx.quadraticCurveTo(leftX, carY - carH * 0.06, leftX + carW * 0.18, carY);
  ctx.quadraticCurveTo(leftX + carW * 0.34, carY + carH * 0.06, leftX + carW * 0.28, carY + carH * 0.13);
  ctx.lineTo(leftX + carW * 0.34, carY + carH * 0.82);
  ctx.quadraticCurveTo(leftX + carW * 0.28, carY + carH, leftX, carY + carH);
  ctx.quadraticCurveTo(leftX - carW * 0.28, carY + carH, leftX - carW * 0.34, carY + carH * 0.82);
  ctx.closePath();
  ctx.fillStyle = '#EEF1F6';
  ctx.fill();

  ctx.fillStyle = '#263244';
  ctx.beginPath();
  ctx.moveTo(leftX - carW * 0.22, carY + carH * 0.22);
  ctx.lineTo(leftX + carW * 0.22, carY + carH * 0.22);
  ctx.lineTo(leftX + carW * 0.18, carY + carH * 0.48);
  ctx.lineTo(leftX - carW * 0.18, carY + carH * 0.48);
  ctx.closePath();
  ctx.fill();

  ctx.strokeStyle = '#536070';
  ctx.lineWidth = 4 * u;
  ctx.beginPath();
  ctx.moveTo(leftX - carW * 0.30, carY + carH * 0.12);
  ctx.lineTo(leftX - carW * 0.34, carY + carH * 0.32);
  ctx.moveTo(leftX + carW * 0.30, carY + carH * 0.12);
  ctx.lineTo(leftX + carW * 0.34, carY + carH * 0.32);
  ctx.stroke();

  ctx.strokeStyle = '#E82127';
  ctx.lineWidth = 5 * u;
  ctx.beginPath();
  ctx.arc(leftX, carY + carH * 0.88, carW * 0.40, 0.12 * Math.PI, 0.88 * Math.PI);
  ctx.stroke();

  text(ctx, '(H)', leftX, H * 0.73, 34 * u, '#FFFFFF', { bold: true });

  // 中央深色玻璃速度环
  const glow = ctx.createRadialGradient(cx, cy, ringR * 0.15, cx, cy, ringR);
  glow.addColorStop(0, 'rgba(20,31,48,0.96)');
  glow.addColorStop(0.72, 'rgba(28,42,61,0.92)');
  glow.addColorStop(1, 'rgba(97,116,136,0.72)');
  ctx.beginPath();
  ctx.arc(cx, cy, ringR, 0, Math.PI * 2);
  ctx.fillStyle = glow;
  ctx.fill();
  ctx.strokeStyle = 'rgba(155,175,190,0.28)';
  ctx.lineWidth = 2 * u;
  ctx.stroke();

  const speed = m.drive.speedText;
  text(ctx, speed, cx, cy - 34 * u, 92 * u, '#FFFFFF', { bold: true });
  text(ctx, 'KM/H', cx, cy + 28 * u, 28 * u, '#8A93A6');

  ctx.beginPath();
  ctx.arc(cx, cy + ringR * 0.60, 38 * u, 0, Math.PI * 2);
  ctx.fillStyle = '#263B52';
  ctx.fill();
  ctx.strokeStyle = '#49C4FF';
  ctx.lineWidth = 2 * u;
  ctx.stroke();
  text(ctx, m.drive.gearText || 'P', cx, cy + ringR * 0.60, 40 * u, '#F7F9FC', { bold: true });

  // 右侧门状态俯视图
  const rightX = W * 0.87;
  ctx.fillStyle = '#0D1526';
  ctx.fillRect(W * 0.72, H * 0.08, W * 0.27, H * 0.62);
  ctx.strokeStyle = '#26304A';
  ctx.lineWidth = 1 * u;
  ctx.strokeRect(W * 0.72, H * 0.08, W * 0.01, H * 0.62);

  const doorW = 78 * u;
  const doorH = 232 * u;
  const doorY = H * 0.18;
  ctx.strokeStyle = '#8A8FA0';
  ctx.lineWidth = 7 * u;
  ctx.beginPath();
  ctx.moveTo(rightX - doorW * 0.46, doorY + doorH * 0.18);
  ctx.quadraticCurveTo(rightX - doorW * 0.46, doorY, rightX, doorY);
  ctx.quadraticCurveTo(rightX + doorW * 0.46, doorY, rightX + doorW * 0.46, doorY + doorH * 0.18);
  ctx.moveTo(rightX - doorW * 0.46, doorY + doorH * 0.82);
  ctx.quadraticCurveTo(rightX - doorW * 0.46, doorY + doorH, rightX, doorY + doorH);
  ctx.quadraticCurveTo(rightX + doorW * 0.46, doorY + doorH, rightX + doorW * 0.46, doorY + doorH * 0.82);
  ctx.stroke();

  ctx.lineWidth = 28 * u;
  ctx.lineCap = 'butt';
  ctx.strokeStyle = '#171C2B';
  [-0.34, 0.34].forEach((offset) => {
    ctx.beginPath();
    ctx.moveTo(rightX + offset * doorW, doorY + doorH * 0.24);
    ctx.lineTo(rightX + offset * doorW, doorY + doorH * 0.76);
    ctx.stroke();
  });

  text(ctx, m.locked === true ? '设备已锁定' : (m.locked === false ? '设备未锁定' : '设备未连接'), rightX, H * 0.73, 30 * u, '#F7F9FC', { bold: true });

  // 底部续航条
  const bottomY = H * 0.87;
  ctx.strokeStyle = 'rgba(255,255,255,0.045)';
  ctx.lineWidth = 1 * u;
  ctx.beginPath();
  ctx.moveTo(W * 0.72, bottomY - 20 * u);
  ctx.lineTo(W * 0.96, bottomY - 20 * u);
  ctx.stroke();
  text(ctx, m.charge.socText, W * 0.755, bottomY, 30 * u, '#FFFFFF', { bold: true });
  text(ctx, '预估可行驶里程', W * 0.82, bottomY + 20 * u, 20 * u, '#A7ADBA');
  text(ctx, m.charge.rangeText + ' km', W * 0.94, bottomY, 26 * u, '#FFFFFF', { bold: true, align: 'right' });
}

// ── 总入口 ─────────────────────────────────────────────────────────
function drawFrame(ctx, W, H, model, tick) {
  drawCockpit(ctx, W, H, model);
}

module.exports = { drawFrame, speedColor };
