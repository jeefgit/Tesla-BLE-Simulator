"""域路由与业务应答（解密后的应用层处理）。

只覆盖仪表场景需要的最小命令集，其余命令返回明确错误，不静默丢弃。
"""

from __future__ import annotations

import hashlib
import time
from dataclasses import dataclass

from .pb import car_server_pb2, common_pb2, errors_pb2, keys_pb2, vcsec_pb2, vehicle_pb2
from .session import DOMAIN_INFOTAINMENT, DOMAIN_VEHICLE_SECURITY

# UniversalMessage.MessageFault_E
FAULT_NONE = 0
FAULT_INVALID_COMMAND = 9
FAULT_DECODING = 10
FAULT_INVALID_DOMAINS = 8


@dataclass
class DispatchResult:
    plaintext: bytes
    fault: int = FAULT_NONE


class Dispatcher:
    def __init__(self, vin: str, telemetry=None, logger=None):
        self.vin = vin
        self.telemetry = telemetry
        self.log = logger or (lambda msg: None)
        self.climate_on = False

    # ── 入口 ──────────────────────────────────────────────
    def handle(self, domain: int, plaintext: bytes, client_public: bytes = b"") -> DispatchResult:
        if domain == DOMAIN_INFOTAINMENT:
            return self._handle_infotainment(plaintext)
        if domain == DOMAIN_VEHICLE_SECURITY:
            return self._handle_vcsec(plaintext, client_public)
        return DispatchResult(b"", FAULT_INVALID_DOMAINS)

    def _data(self) -> dict:
        if self.telemetry is None:
            return {}
        try:
            return self.telemetry.get_data()
        except Exception:
            return {}

    def _update(self, **kwargs) -> None:
        if self.telemetry is None:
            return
        try:
            self.telemetry.update_data(**kwargs)
        except Exception:
            pass

    # ── Infotainment（CarServer.Action）────────────────────
    def _handle_infotainment(self, plaintext: bytes) -> DispatchResult:
        action = car_server_pb2.Action()
        try:
            action.ParseFromString(plaintext)
        except Exception as exc:
            self.log(f"[FAULT] CarServer.Action 解析失败: {exc}")
            return DispatchResult(b"", FAULT_DECODING)

        if not action.HasField("vehicleAction"):
            return self._ok_response()

        va = action.vehicleAction
        kind = va.WhichOneof("vehicle_action_msg")

        if kind == "getVehicleData":
            return self._handle_get_vehicle_data(va.getVehicleData)
        if kind == "chargingStartStopAction":
            return self._handle_charge_start_stop(va.chargingStartStopAction)
        if kind == "hvacAutoAction":
            self.climate_on = bool(va.hvacAutoAction.power_on)
            self.log(f"[GCM] HVAC {'ON' if self.climate_on else 'OFF'}")
            return self._ok_response()
        if kind == "chargingSetLimitAction":
            self.log(f"[GCM] 充电上限设为 {va.chargingSetLimitAction.percent}%")
            return self._ok_response()
        if kind == "vehicleControlHonkHornAction":
            self.log("[GCM] 鸣笛命令")
            return self._ok_response()
        if kind == "ping":
            resp = car_server_pb2.Response()
            resp.actionStatus.result = car_server_pb2.OPERATIONSTATUS_OK
            resp.ping.ping_id = va.ping.ping_id
            return DispatchResult(resp.SerializeToString())

        self.log(f"[FAULT] 未支持的 Infotainment 命令: {kind}")
        return self._error_response(f"unsupported command: {kind}")

    def _handle_get_vehicle_data(self, gvd) -> DispatchResult:
        want = {
            name
            for name in (
                "getChargeState",
                "getClimateState",
                "getDriveState",
                "getClosuresState",
                "getTirePressureState",
                "getLocationState",
            )
            if gvd.HasField(name)
        }
        if not want:
            want = {
                "getChargeState",
                "getClimateState",
                "getDriveState",
                "getClosuresState",
                "getTirePressureState",
            }

        resp = car_server_pb2.Response()
        resp.actionStatus.result = car_server_pb2.OPERATIONSTATUS_OK
        vd = resp.vehicleData
        data = self._data()
        now = int(time.time())

        if "getDriveState" in want:
            self._fill_drive_state(vd.drive_state, data, now)
        if "getChargeState" in want:
            self._fill_charge_state(vd.charge_state, data, now)
        if "getClimateState" in want:
            self._fill_climate_state(vd.climate_state, data, now)
        if "getClosuresState" in want:
            self._fill_closures_state(vd.closures_state, data, now)
        if "getTirePressureState" in want:
            self._fill_tire_pressure_state(vd.tire_pressure_state, data, now)
        if "getLocationState" in want:
            self._fill_location_state(vd.location_state, now)

        return DispatchResult(resp.SerializeToString())

    # ── 各状态填充 ────────────────────────────────────────
    @staticmethod
    def _stamp(msg, now: int) -> None:
        msg.timestamp.seconds = now

    def _fill_drive_state(self, ds, data: dict, now: int) -> None:
        gear = str(data.get("gear", "P"))[:1].upper()
        gear_field = {"P": "P", "R": "R", "N": "N", "D": "D"}.get(gear, "SNA")
        getattr(ds.shift_state, gear_field).SetInParent()
        ds.speed_float = float(data.get("speed_kmh", 0.0))
        ds.speed = int(max(0, min(255, round(float(data.get("speed_kmh", 0.0)) * 0.621371))))
        ds.power = int(round(float(data.get("motor_power_kw", 0.0))))
        ds.timestamp.seconds = now

    def _fill_charge_state(self, cs, data: dict, now: int) -> None:
        charging = bool(data.get("charging", False))
        if charging:
            cs.charging_state.Charging.SetInParent()
        else:
            cs.charging_state.Disconnected.SetInParent()
        cs.battery_level = int(data.get("battery_level", 0))
        cs.usable_battery_level = int(data.get("battery_level", 0))
        cs.battery_range = float(data.get("battery_range_km", 0.0))
        cs.ideal_battery_range = float(data.get("battery_range_km", 0.0))
        cs.est_battery_range = float(data.get("battery_range_km", 0.0))
        cs.charger_power = int(round(float(data.get("charge_power_kw", 0.0))))
        cs.charge_port_door_open = charging
        cs.charge_limit_soc = 80
        cs.charge_limit_soc_std = 80
        cs.charge_limit_soc_min = 50
        cs.charge_limit_soc_max = 100
        cs.timestamp.seconds = now

    def _fill_climate_state(self, cs, data: dict, now: int) -> None:
        cs.inside_temp_celsius = float(data.get("inside_temp", 0.0))
        cs.outside_temp_celsius = float(data.get("outside_temp", 0.0))
        cs.driver_temp_setting = 22.0
        cs.passenger_temp_setting = 22.0
        cs.is_climate_on = self.climate_on
        cs.is_auto_conditioning_on = self.climate_on
        cs.fan_status = 3 if self.climate_on else 0
        cs.min_avail_temp_celsius = 15.0
        cs.max_avail_temp_celsius = 28.0
        cs.timestamp.seconds = now

    def _fill_closures_state(self, cl, data: dict, now: int) -> None:
        cl.door_open_driver_front = bool(data.get("door_open_fl", False))
        cl.door_open_passenger_front = bool(data.get("door_open_fr", False))
        cl.door_open_driver_rear = bool(data.get("door_open_rl", False))
        cl.door_open_passenger_rear = bool(data.get("door_open_rr", False))
        cl.door_open_trunk_front = bool(data.get("door_open_trunk_front", False))
        cl.door_open_trunk_rear = bool(data.get("door_open_trunk_rear", False))
        cl.locked = bool(data.get("locked", False))
        cl.timestamp.seconds = now

    def _fill_tire_pressure_state(self, tp, data: dict, now: int) -> None:
        tp.tpms_pressure_fl = float(data.get("tpms_fl", 0.0))
        tp.tpms_pressure_fr = float(data.get("tpms_fr", 0.0))
        tp.tpms_pressure_rl = float(data.get("tpms_rl", 0.0))
        tp.tpms_pressure_rr = float(data.get("tpms_rr", 0.0))
        tp.timestamp.seconds = now

    def _fill_location_state(self, ls, now: int) -> None:
        ls.latitude = 0.0
        ls.longitude = 0.0
        ls.heading = 0
        ls.timestamp.seconds = now

    def _handle_charge_start_stop(self, action) -> DispatchResult:
        kind = action.WhichOneof("charging_action") or "unknown"
        if kind in ("start", "start_standard", "start_max_range"):
            self._update(charging=True, charge_power_kw=7.2)
            self.log("[GCM] 开始充电")
        elif kind == "stop":
            self._update(charging=False, charge_power_kw=0.0)
            self.log("[GCM] 停止充电")
        else:
            self.log(f"[GCM] 充电命令: {kind}")
        return self._ok_response()

    @staticmethod
    def _ok_response() -> DispatchResult:
        resp = car_server_pb2.Response()
        resp.actionStatus.result = car_server_pb2.OPERATIONSTATUS_OK
        return DispatchResult(resp.SerializeToString())

    @staticmethod
    def _error_response(text: str) -> DispatchResult:
        resp = car_server_pb2.Response()
        resp.actionStatus.result = car_server_pb2.OPERATIONSTATUS_ERROR
        resp.actionStatus.result_reason.plain_text = text
        return DispatchResult(resp.SerializeToString(), FAULT_INVALID_COMMAND)

    # ── VCSEC（UnsignedMessage）──────────────────────────
    def _handle_vcsec(self, plaintext: bytes, client_public: bytes) -> DispatchResult:
        msg = vcsec_pb2.UnsignedMessage()
        try:
            msg.ParseFromString(plaintext)
        except Exception as exc:
            self.log(f"[FAULT] VCSEC.UnsignedMessage 解析失败: {exc}")
            return DispatchResult(b"", FAULT_DECODING)

        kind = msg.WhichOneof("sub_message")
        reply = vcsec_pb2.FromVCSECMessage()

        if kind == "InformationRequest":
            req_type = msg.InformationRequest.informationRequestType
            if req_type == vcsec_pb2.INFORMATION_REQUEST_TYPE_GET_STATUS:
                self._fill_vcsec_status(reply.vehicleStatus)
                return DispatchResult(reply.SerializeToString())
            if req_type == vcsec_pb2.INFORMATION_REQUEST_TYPE_GET_WHITELIST_INFO:
                info = reply.whitelistInfo
                info.numberOfEntries = 1
                if client_public:
                    info.whitelistEntries.add().publicKeySHA1 = hashlib.sha1(
                        client_public
                    ).digest()
                info.slotMask = 0x01
                return DispatchResult(reply.SerializeToString())
            if req_type == vcsec_pb2.INFORMATION_REQUEST_TYPE_GET_WHITELIST_ENTRY_INFO:
                entry = reply.whitelistEntryInfo
                if client_public:
                    entry.keyId.publicKeySHA1 = hashlib.sha1(client_public).digest()
                    entry.publicKey.PublicKeyRaw = client_public
                entry.metadataForKey.keyFormFactor = vcsec_pb2.KEY_FORM_FACTOR_ANDROID_DEVICE
                entry.slot = 1
                entry.keyRole = keys_pb2.ROLE_OWNER
                return DispatchResult(reply.SerializeToString())
            self.log(f"[FAULT] 未支持的 VCSEC 信息请求: {req_type}")
            reply.nominalError.genericError = errors_pb2.GENERICERROR_UNKNOWN
            return DispatchResult(reply.SerializeToString(), FAULT_INVALID_COMMAND)

        if kind == "RKEAction":
            action = msg.RKEAction
            if action == vcsec_pb2.RKE_ACTION_LOCK:
                self._update(locked=True)
                self.log("[GCM] VCSEC 上锁")
            elif action == vcsec_pb2.RKE_ACTION_UNLOCK:
                self._update(locked=False)
                self.log("[GCM] VCSEC 解锁")
            reply.commandStatus.operationStatus = vcsec_pb2.OPERATIONSTATUS_OK
            return DispatchResult(reply.SerializeToString())

        if kind == "closureMoveRequest":
            self._apply_closure_moves(msg.closureMoveRequest)
            reply.commandStatus.operationStatus = vcsec_pb2.OPERATIONSTATUS_OK
            return DispatchResult(reply.SerializeToString())

        if kind == "WhitelistOperation":
            reply.commandStatus.operationStatus = vcsec_pb2.OPERATIONSTATUS_OK
            reply.commandStatus.whitelistOperationStatus.operationStatus = (
                vcsec_pb2.OPERATIONSTATUS_OK
            )
            self.log("[GCM] VCSEC 白名单操作（模拟器直接放行）")
            return DispatchResult(reply.SerializeToString())

        self.log(f"[FAULT] 未支持的 VCSEC 命令: {kind}")
        reply.nominalError.genericError = errors_pb2.GENERICERROR_UNKNOWN
        return DispatchResult(reply.SerializeToString(), FAULT_INVALID_COMMAND)

    def _fill_vcsec_status(self, status) -> None:
        data = self._data()
        closures = status.closureStatuses
        mapping = (
            ("frontDriverDoor", "door_open_fl"),
            ("frontPassengerDoor", "door_open_fr"),
            ("rearDriverDoor", "door_open_rl"),
            ("rearPassengerDoor", "door_open_rr"),
            ("frontTrunk", "door_open_trunk_front"),
            ("rearTrunk", "door_open_trunk_rear"),
        )
        for field_name, key in mapping:
            is_open = bool(data.get(key, False))
            setattr(
                closures,
                field_name,
                vcsec_pb2.CLOSURESTATE_OPEN if is_open else vcsec_pb2.CLOSURESTATE_CLOSED,
            )
        status.vehicleLockState = (
            vcsec_pb2.VEHICLELOCKSTATE_LOCKED
            if data.get("locked", False)
            else vcsec_pb2.VEHICLELOCKSTATE_UNLOCKED
        )
        status.vehicleSleepStatus = vcsec_pb2.VEHICLE_SLEEP_STATUS_AWAKE
        status.userPresence = vcsec_pb2.VEHICLE_USER_PRESENCE_UNKNOWN

    def _apply_closure_moves(self, moves) -> None:
        mapping = (
            ("frontDriverDoor", "door_open_fl"),
            ("frontPassengerDoor", "door_open_fr"),
            ("rearDriverDoor", "door_open_rl"),
            ("rearPassengerDoor", "door_open_rr"),
            ("frontTrunk", "door_open_trunk_front"),
            ("rearTrunk", "door_open_trunk_rear"),
        )
        updates = {}
        for field_name, key in mapping:
            value = getattr(moves, field_name)
            if value == vcsec_pb2.CLOSURE_MOVE_TYPE_OPEN:
                updates[key] = True
            elif value == vcsec_pb2.CLOSURE_MOVE_TYPE_CLOSE:
                updates[key] = False
        if updates:
            self._update(**updates)
            self.log(f"[GCM] VCSEC 车门动作: {updates}")
