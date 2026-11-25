// 53 字节 SimPayload 解码（小端序，与 sim_server.py PAYLOAD_FORMAT "<ffifc6BBBfff4f" 一致）
//   0-3 speed_kmh f32 | 4-7 motor_power_kw f32 | 8-11 battery_level i32 | 12-15 battery_range_km f32
//   16 gear char | 17-22 doors[6]=fl,fr,rl,rr,frunk,trunk | 23 locked u8 | 24 charging u8
//   25-28 charge_power_kw f32 | 29-32 inside_temp f32 | 33-36 outside_temp f32 | 37-52 tpms[4] f32
'use strict';

const { CFG, SIM_PAYLOAD_LEN } = require('../config/index');
const { readF32le, readI32le } = require('../utils/bytes');

function decodeSimPayload(bytes) {
  if (!bytes || bytes.length !== SIM_PAYLOAD_LEN) return null;
  const d = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const out = {
    speedKmh: d.getFloat32(0, true),
    motorPowerKw: d.getFloat32(4, true),
    batteryLevel: d.getInt32(8, true),
    batteryRangeKm: d.getFloat32(12, true),
    gear: String.fromCharCode(bytes[16]),
    doors: [],
    locked: bytes[23] !== 0,
    charging: bytes[24] !== 0,
    chargePowerKw: d.getFloat32(25, true),
    insideTemp: d.getFloat32(29, true),
    outsideTemp: d.getFloat32(33, true),
    tpms: [],
  };
  for (let i = 0; i < 6; i++) out.doors.push(bytes[17 + i] !== 0);
  for (let i = 0; i < 4; i++) out.tpms.push(d.getFloat32(37 + i * 4, true));

  // 合理性校验：任一关键字段 NaN 视为坏帧
  const nums = [out.speedKmh, out.motorPowerKw, out.batteryLevel, out.batteryRangeKm,
    out.chargePowerKw, out.insideTemp, out.outsideTemp].concat(out.tpms);
  for (let i = 0; i < nums.length; i++) {
    if (typeof nums[i] !== 'number' || isNaN(nums[i])) return null;
  }
  return out;
}

// 反向编码（联调自测用）
function encodeSimPayload(d) {
  const out = new Uint8Array(SIM_PAYLOAD_LEN);
  const dv = new DataView(out.buffer);
  dv.setFloat32(0, d.speedKmh, true);
  dv.setFloat32(4, d.motorPowerKw, true);
  dv.setInt32(8, d.batteryLevel | 0, true);
  dv.setFloat32(12, d.batteryRangeKm, true);
  out[16] = String(d.gear || '?').charCodeAt(0) & 0xff;
  for (let i = 0; i < 6; i++) out[17 + i] = d.doors && d.doors[i] ? 1 : 0;
  out[23] = d.locked ? 1 : 0;
  out[24] = d.charging ? 1 : 0;
  dv.setFloat32(25, d.chargePowerKw, true);
  dv.setFloat32(29, d.insideTemp, true);
  dv.setFloat32(33, d.outsideTemp, true);
  for (let i = 0; i < 4; i++) dv.setFloat32(37 + i * 4, d.tpms[i], true);
  return out;
}

module.exports = { decodeSimPayload, encodeSimPayload };
