// CarServer.Response 解码（解密后的 protobuf 明文 → 车辆数据片段）
// 字段号来源：simulator/tesla_protocol/protobuf/{car_server,vehicle}.proto
// 与 ESP32 TeslaClient.cpp decode{Charge,Climate,Drive,Closures,TirePressure}State
'use strict';

const { PbReader } = require('./pbCodec');

// VehicleData 子状态字段号（vehicle.proto VehicleData）
const F_CHARGE = 3;
const F_CLIMATE = 4;
const F_DRIVE = 5;
const F_CLOSURES = 9;
const F_TIRE = 19;

// 解析结果：{ ok, actionOk, actionError, vehicleData:{ drive?, charge?, climate?, closures?, tirePressure? } }
// 单位保持车端原始值：speed mph、range mile（由调用方换算 km）
function parseCarServerResponse(bytes) {
  const out = { ok: false, actionOk: null, actionError: null, vehicleData: null };

  // 顶层：Response{ actionStatus=1, vehicleData=2 }
  const r = new PbReader(bytes);
  while (!r.atEnd()) {
    const h = r.next();
    if (!h) return out;
    if (h.field === 1 && h.wire === 2) {
      const d = r.readBytes();
      if (!d) return out;
      const st = parseActionStatus(d);
      if (st) {
        out.actionOk = st.ok;
        out.actionError = st.error;
      }
    } else if (h.field === 2 && h.wire === 2) {
      const d = r.readBytes();
      if (!d) return out;
      out.vehicleData = parseVehicleData(d);
    } else if (!r.skip(h.wire)) {
      return out;
    }
  }
  out.ok = out.vehicleData !== null || out.actionOk !== null;
  return out;
}

// ActionStatus{ result=1, result_reason=2 }
function parseActionStatus(bytes) {
  const r = new PbReader(bytes);
  let ok = null;
  while (!r.atEnd()) {
    const h = r.next();
    if (!h) return null;
    if (h.field === 1 && h.wire === 0) {
      const v = r.readVarint();
      if (v === null) return null;
      ok = (v >>> 0) === 0; // OPERATIONSTATUS_OK = 0
    } else if (!r.skip(h.wire)) {
      return null;
    }
  }
  return { ok, error: ok === false ? 'actionStatus ERROR' : null };
}

function parseVehicleData(bytes) {
  const vd = {};
  const r = new PbReader(bytes);
  while (!r.atEnd()) {
    const h = r.next();
    if (!h) return null;
    const d = (h.wire === 2) ? r.readBytes() : (r.skip(h.wire), null);
    if (h.wire === 2 && !d) return null;
    if (h.field === F_CHARGE && d) {
      vd.charge = parseChargeState(d);
    } else if (h.field === F_CLIMATE && d) {
      vd.climate = parseClimateState(d);
    } else if (h.field === F_DRIVE && d) {
      vd.drive = parseDriveState(d);
    } else if (h.field === F_CLOSURES && d) {
      vd.closures = parseClosuresState(d);
    } else if (h.field === F_TIRE && d) {
      vd.tirePressure = parseTirePressureState(d);
    }
  }
  return vd;
}

// DriveState：shift_state=1（oneof: P=2/R=3/N=4/D=5/SNA=6），speed=102 mph int，
// speed_float=106 mph float，power=103 int32（负=动能回收）
function parseDriveState(bytes) {
  const out = {};
  const r = new PbReader(bytes);
  while (!r.atEnd()) {
    const h = r.next();
    if (!h) return null;
    if (h.field === 1 && h.wire === 2) {
      const d = r.readBytes();
      if (!d) return null;
      const gear = parseShiftState(d);
      if (gear) out.gear = gear;
    } else if (h.field === 106 && h.wire === 5) {
      out.speedMph = r.readFloat32();
      if (out.speedMph === null) return null;
    } else if (h.field === 102 && h.wire === 0) {
      // 旧字段兜底（无 speed_float 时才生效，与 C++ 一致）
      const v = r.readVarint();
      if (v === null) return null;
      if (out.speedMph === undefined) out.speedMph = v >>> 0;
    } else if (h.field === 103 && h.wire === 0) {
      out.powerKw = r.readInt32();
      if (out.powerKw === null) return null;
    } else if (!r.skip(h.wire)) {
      return null;
    }
  }
  return out;
}

// ShiftState oneof 子消息
function parseShiftState(bytes) {
  const r = new PbReader(bytes);
  let gear = '?';
  let found = false;
  while (!r.atEnd()) {
    const h = r.next();
    if (!h) return null;
    if (h.wire === 2) {
      const d = r.readBytes();
      if (!d) return null;
      found = true;
      gear = h.field === 2 ? 'P' : h.field === 3 ? 'R' : h.field === 4 ? 'N' : h.field === 5 ? 'D' : '?';
    } else if (!r.skip(h.wire)) {
      return null;
    }
  }
  return found ? gear : null;
}

// ChargeState：charging_state=1（oneof: Starting=4/Charging=5 → 充电中），
// battery_range=111 / est=112 / ideal=113 (mile float)，battery_level=114，
// usable=115，charger_power=122
function parseChargeState(bytes) {
  const out = {};
  let hasSoc = false;
  const r = new PbReader(bytes);
  while (!r.atEnd()) {
    const h = r.next();
    if (!h) return null;
    if (h.field === 1 && h.wire === 2) {
      const d = r.readBytes();
      if (!d) return null;
      const charging = parseChargingState(d);
      if (charging !== null) out.charging = charging;
    } else if (h.field === 111 && h.wire === 5) {
      out.rangeMiles = r.readFloat32();
      if (out.rangeMiles === null) return null;
    } else if (h.field === 112 && h.wire === 5) {
      const v = r.readFloat32();
      if (v === null) return null;
      if (out.rangeMiles === undefined) out.rangeMiles = v; // est 兜底
    } else if (h.field === 113 && h.wire === 5) {
      const v = r.readFloat32();
      if (v === null) return null;
      if (out.rangeMiles === undefined) out.rangeMiles = v; // ideal 兜底
    } else if (h.field === 114 && h.wire === 0) {
      const v = r.readVarint();
      if (v === null) return null;
      out.soc = v >>> 0;
      hasSoc = true;
    } else if (h.field === 115 && h.wire === 0) {
      const v = r.readVarint();
      if (v === null) return null;
      if (!hasSoc) out.soc = v >>> 0;
    } else if (h.field === 122 && h.wire === 0) {
      out.chargePowerKw = r.readInt32();
      if (out.chargePowerKw === null) return null;
    } else if (!r.skip(h.wire)) {
      return null;
    }
  }
  return out;
}

// ChargingState oneof：空消息按字段号区分（Starting=4 / Charging=5 → true）
function parseChargingState(bytes) {
  const r = new PbReader(bytes);
  let charging = false;
  while (!r.atEnd()) {
    const h = r.next();
    if (!h) return null;
    if (h.wire === 2) {
      const d = r.readBytes();
      if (!d) return null;
      if (h.field === 4 || h.field === 5) charging = true;
    } else if (!r.skip(h.wire)) {
      return null;
    }
  }
  return charging;
}

// ClimateState：inside_temp_celsius=101，outside_temp_celsius=102（float）
function parseClimateState(bytes) {
  const out = {};
  const r = new PbReader(bytes);
  while (!r.atEnd()) {
    const h = r.next();
    if (!h) return null;
    if (h.field === 101 && h.wire === 5) {
      out.insideTemp = r.readFloat32();
      if (out.insideTemp === null) return null;
    } else if (h.field === 102 && h.wire === 5) {
      out.outsideTemp = r.readFloat32();
      if (out.outsideTemp === null) return null;
    } else if (!r.skip(h.wire)) {
      return null;
    }
  }
  return out;
}

// ClosuresState：door_open_driver_front=101(FL)，driver_rear=102(RL)，
// passenger_front=103(FR)，passenger_rear=104(RR)，trunk_front=105(FRUNK)，
// trunk_rear=106(TRUNK)，locked=113（bool varint，索引映射与 C++ decodeClosuresState 一致）
function parseClosuresState(bytes) {
  const out = {};
  const doors = [null, null, null, null, null, null];
  const doorFields = { 101: 0, 103: 1, 102: 2, 104: 3, 105: 4, 106: 5 };
  const r = new PbReader(bytes);
  while (!r.atEnd()) {
    const h = r.next();
    if (!h) return null;
    if (doorFields[h.field] !== undefined && h.wire === 0) {
      const v = r.readVarint();
      if (v === null) return null;
      doors[doorFields[h.field]] = v !== 0;
    } else if (h.field === 113 && h.wire === 0) {
      const v = r.readVarint();
      if (v === null) return null;
      out.locked = v !== 0;
    } else if (!r.skip(h.wire)) {
      return null;
    }
  }
  let any = false;
  for (let i = 0; i < 6; i++) if (doors[i] !== null) any = true;
  if (any) out.doors = doors;
  return out;
}

// TirePressureState：tpms_pressure_fl/fr/rl/rr = 2..5（float，bar）
function parseTirePressureState(bytes) {
  const out = {};
  const tp = [null, null, null, null];
  const r = new PbReader(bytes);
  while (!r.atEnd()) {
    const h = r.next();
    if (!h) return null;
    if (h.field >= 2 && h.field <= 5 && h.wire === 5) {
      const v = r.readFloat32();
      if (v === null) return null;
      tp[h.field - 2] = v;
    } else if (!r.skip(h.wire)) {
      return null;
    }
  }
  if (tp[0] !== null || tp[1] !== null || tp[2] !== null || tp[3] !== null) out.tpms = tp;
  return out;
}

module.exports = { parseCarServerResponse };
