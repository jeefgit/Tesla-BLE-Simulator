import sys
import os
import time
import math
import struct
import argparse
import asyncio
import threading
import queue
import hashlib
from datetime import datetime

_VENDOR_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "vendor")
if os.path.isdir(_VENDOR_DIR) and _VENDOR_DIR not in sys.path:
    sys.path.insert(0, _VENDOR_DIR)

# BLE 模拟库导入保护
BLESS_IMPORT_ERROR = None
try:
    from bless import (
        BlessServer,
        BlessGATTCharacteristic,
        GATTCharacteristicProperties,
        GATTAttributePermissions
    )
except ImportError as e:
    BlessServer = None
    BLESS_IMPORT_ERROR = e

BUMBLE_IMPORT_ERROR = None
try:
    from bumble.device import Device as BumbleDevice
    from bumble.gatt import Service as BumbleService, Characteristic as BumbleChar
    from bumble.core import UUID as BumbleUUID
    from bumble.transport import open_transport as bumble_open_transport
    from bumble.host import Host as BumbleHost
    from bumble.hci import OwnAddressType
    HAS_BUMBLE = True
except ImportError as e:
    HAS_BUMBLE = False
    BUMBLE_IMPORT_ERROR = e

# 特斯拉真实加密协议栈（ECDH + AES-GCM + HMAC）
TESLA_CRYPTO_ERROR = None
try:
    from tesla_protocol.server import TeslaCryptoServer
except ImportError as e:
    TeslaCryptoServer = None
    TESLA_CRYPTO_ERROR = e

class BumbleTeslaServer:
    def __init__(self, vin="LRWYGCFJ8RC571213", usb_spec="usb:0a12:0001"):
        self.vin = vin
        self.usb_spec = usb_spec
        sha1 = hashlib.sha1(vin.encode('utf-8')).hexdigest()
        self.ble_name = "S" + sha1[:16] + "C"
        
        self.device = None
        self.host = None
        self.transport = None
        self.notify_char = None
        self.write_char = None
        self.connected_peers = set()
        self.write_callback = None

    async def start(self):
        print(f"[Bumble] 正在初始化 CSR8510 USB 蓝牙适配器 ({self.usb_spec})...")
        self.transport = await bumble_open_transport(self.usb_spec)
        self.host = BumbleHost(self.transport.source, self.transport.sink)
        
        self.device = BumbleDevice(
            address='F4:4E:FC:00:00:01',
            name=self.ble_name,
            host=self.host
        )
        
        self.device.advertising_data = bytes(
            [2, 0x01, 0x06] +
            [17, 0x06] + list(bytes.fromhex('1eb9f8eb0c96889bf043d1b211020000'))
        )
        self.device.scan_response_data = bytes(
            [len(self.ble_name) + 1, 0x09] + list(bytes(self.ble_name, 'utf-8'))
        )
        
        self.write_char = BumbleChar(
            BumbleUUID(WR_UUID),
            BumbleChar.Properties.WRITE | BumbleChar.Properties.WRITE_WITHOUT_RESPONSE,
            BumbleChar.Permissions.WRITEABLE,
            bytes()
        )
        
        if self.write_callback:
            @self.write_char.on('write')
            def _on_write(connection, value):
                self.write_callback(self.write_char, value)

        self.notify_char = BumbleChar(
            BumbleUUID(RD_UUID),
            BumbleChar.Properties.READ | BumbleChar.Properties.NOTIFY,
            BumbleChar.Permissions.READABLE,
            bytes()
        )
        
        service = BumbleService(BumbleUUID(SVC_UUID), [self.write_char, self.notify_char])
        self.device.add_service(service)
        
        @self.device.on('connection')
        def on_connection(connection):
            print(f"[Bumble] ESP32 已建立物理层 BLE 连接: {connection}")
            self.connected_peers.add(connection)
            
        @self.device.on('disconnection')
        def on_disconnection(connection, reason):
            print(f"[Bumble] ESP32 断开 BLE 连接, 原因代码: {reason}")
            self.connected_peers.discard(connection)
            if self.device and self.transport:
                asyncio.create_task(self._safe_restart_advertising())

        await self.device.power_on()
        self.device.gatt_server.max_mtu = 256
        # BLE 4.0 芯片在连接建立后不支持同时广播，auto_restart 设为 False 避免硬件返回 COMMAND_DISALLOWED
        await self.device.start_advertising(own_address_type=OwnAddressType.PUBLIC, auto_restart=False)
        print(f"[Bumble] [+] 特斯拉 BLE 空中广播启动成功！广播名: {self.ble_name}")

    async def _safe_restart_advertising(self):
        try:
            await asyncio.sleep(0.5)
            if self.device and self.transport and len(self.connected_peers) == 0:
                await self.device.start_advertising(own_address_type=OwnAddressType.PUBLIC, auto_restart=False)
                print(f"[Bumble] [+] ESP32 断开后已重新自动拉起 BLE 广播: {self.ble_name}")
        except Exception:
            pass

    async def is_connected(self):
        return len(self.connected_peers) > 0

    async def notify(self, payload_bytes):
        if self.notify_char and self.connected_peers:
            for conn in list(self.connected_peers):
                await self.device.notify_subscriber(conn, self.notify_char, payload_bytes)

    async def stop(self):
        if self.device:
            try:
                await self.device.stop_advertising()
            except Exception:
                pass
        if self.transport:
            try:
                await self.transport.close()
                print("[Bumble] USB HCI 接口已安全关闭并释放。")
            except Exception as e:
                print(f"[Bumble] 关闭 USB 接口提示: {e}")
            self.transport = None
        self.device = None
        self.host = None
        self.connected_peers.clear()

# Apply monkey patch to Bless WinRT BLEAdapter to handle name change hardware reset delay
if BlessServer is not None:
    try:
        from bless.backends.winrt.ble.adapter import BLEAdapter
        _orig_set_local_name = BLEAdapter.set_local_name
        def patched_set_local_name(self, local_name):
            msg1 = f"[BLE] 正在写入 Windows 蓝牙广播名称: {local_name}"
            msg2 = "[BLE] 已发送重置命令。正在等待 4 秒以使 Windows 蓝牙适配器重新载入新名称..."
            print(msg1)
            print(msg2)
            try:
                if 'log_queue' in globals():
                    globals()['log_queue'].put(msg1)
                    globals()['log_queue'].put(msg2)
            except:
                pass
            _orig_set_local_name(self, local_name)
            time.sleep(4.0)
        BLEAdapter.set_local_name = patched_set_local_name
        print("[BLE] Windows BLEAdapter 延迟补丁应用成功。")
    except Exception as e:
        print(f"[BLE] 警告: 无法应用 BLEAdapter 延迟补丁 ({e})")

# Tkinter 导入保护
try:
    import tkinter as tk
    from tkinter import ttk, messagebox
except ImportError:
    tk = None

import random

# ─── 特斯拉自定义 BLE UUID ──────────────────────────────────────
SVC_UUID = "00000211-b2d1-43f0-9b88-960cebf8b91e"
WR_UUID  = "00000212-b2d1-43f0-9b88-960cebf8b91e"
RD_UUID  = "00000213-b2d1-43f0-9b88-960cebf8b91e"

# ─── 数据载荷打包格式 ──────────────────────────────────────────
# f: speed_kmh, f: motor_power_kw, i: battery_level, f: battery_range_km, c: gear,
# 6B: doors, B: locked, B: charging, f: charge_power_kw, f: inside_temp, f: outside_temp,
# 4f: tpms[4] (53 bytes total)
PAYLOAD_FORMAT = "<ffifc6BBBfff4f"

# ─── 线程安全共享遥测状态 ─────────────────────────────────────
class TelemetryState:
    def __init__(self):
        self.lock = threading.Lock()
        self.use_script_mode = False  # 默认为手动干预模式，应用户要求关闭自动剧本播放
        self.ble_connected = False    # BLE 连接状态
        self.ble_enabled = True       # BLE 蓝牙广播全局总开关 (默认开启)
        self.frame_counter = 0        # 剧本帧计数器
        self.vin = "LRWYGCFJ8RC571213" # 默认 VIN (用户可在界面/命令行自行填入)
        self.request_restart = False  # 蓝牙广播重启标志位

        # 真实车机 BLE 模拟扩展参数
        self.comm_mode = "REALISTIC"   # "REALISTIC" (真实车机问答模式) 或 "IDEAL" (理想20Hz主动推送)
        self.delay_multiplier = 1.0    # 延迟倍率 (0.5x ~ 3.0x)
        self.loss_rate = 0.0           # 模拟丢包率 (0.0 ~ 0.20)
        self.queue_depth = 0           # 实时命令队列深度
        self.fault_drop_until = 0.0    # 故障注入截止时间戳
        self.recent_latencies = []     # 近期响应延迟历史 (ms)

        # 特斯拉加密协议（real path）开关
        self.crypto_enabled = True     # True = 启用真实加密通道；False = 纯 53B 旁路
        self.response_encryption = True  # True = 模拟 2024.38+ 加密响应；False = 老固件明文响应
        self.crypto_reset_pending = False  # GUI「使会话失效」→ 模拟车机重启

        # BLE 配钥（add-key-request）：模拟「中控台轻点 NFC 钥匙卡」授权
        self.pair_approve_pending = False
        self.pair_reject_pending = False
        self.pair_pending_desc = ""      # 当前待授权请求描述（供界面/日志展示）

        # 初始车况数据
        self.data = {
            'speed_kmh': 0.0,
            'motor_power_kw': 0.0,
            'battery_level': 62,
            'battery_range_km': 248.0,
            'gear': 'P',
            'door_open_fl': False,
            'door_open_fr': False,
            'door_open_rl': False,
            'door_open_rr': False,
            'door_open_trunk_front': False,
            'door_open_trunk_rear': False,
            'locked': True,
            'charging': False,
            'charge_power_kw': 0.0,
            'inside_temp': 35.0,
            'outside_temp': 16.0,
            'tpms_fl': 2.3,
            'tpms_fr': 2.3,
            'tpms_rl': 2.3,
            'tpms_rr': 2.3
        }

    def get_data(self):
        with self.lock:
            return self.data.copy()

    def update_data(self, **kwargs):
        with self.lock:
            for k, v in kwargs.items():
                if k in self.data:
                    self.data[k] = v

    def is_script_mode(self):
        with self.lock:
            return self.use_script_mode

    def set_script_mode(self, val):
        with self.lock:
            self.use_script_mode = val

    def set_connected(self, val):
        with self.lock:
            self.ble_connected = val

    def is_connected(self):
        with self.lock:
            return self.ble_connected

    def get_frame(self):
        with self.lock:
            return self.frame_counter

    def increment_frame(self):
        with self.lock:
            self.frame_counter = (self.frame_counter + 1) % 1030
            return self.frame_counter

    def reset_frame(self):
        with self.lock:
            self.frame_counter = 0

    def get_vin(self):
        with self.lock:
            return self.vin

    def set_vin(self, val):
        with self.lock:
            self.vin = val

    def check_restart(self):
        with self.lock:
            val = self.request_restart
            self.request_restart = False
            return val

    def set_restart(self, val):
        with self.lock:
            self.request_restart = val

    def is_ble_enabled(self):
        with self.lock:
            return self.ble_enabled

    def set_ble_enabled(self, val: bool):
        with self.lock:
            self.ble_enabled = val

    def toggle_ble_enabled(self):
        with self.lock:
            self.ble_enabled = not self.ble_enabled
            return self.ble_enabled

    def get_comm_mode(self):
        with self.lock:
            return self.comm_mode

    def set_comm_mode(self, mode):
        with self.lock:
            self.comm_mode = mode

    def toggle_comm_mode(self):
        with self.lock:
            self.comm_mode = "IDEAL" if self.comm_mode == "REALISTIC" else "REALISTIC"
            return self.comm_mode

    def get_delay_multiplier(self):
        with self.lock:
            return self.delay_multiplier

    def set_delay_multiplier(self, val: float):
        with self.lock:
            self.delay_multiplier = float(val)

    def get_loss_rate(self):
        with self.lock:
            return self.loss_rate

    def set_loss_rate(self, val: float):
        with self.lock:
            self.loss_rate = float(val)

    def set_queue_depth(self, val: int):
        with self.lock:
            self.queue_depth = val

    def get_queue_depth(self):
        with self.lock:
            return self.queue_depth

    def inject_fault(self, duration=5.0):
        with self.lock:
            self.fault_drop_until = time.time() + duration

    def get_fault_drop_until(self):
        with self.lock:
            return self.fault_drop_until

    def add_latency_record(self, lat_ms: float):
        with self.lock:
            self.recent_latencies.append(lat_ms)
            if len(self.recent_latencies) > 10:
                self.recent_latencies.pop(0)

    def get_avg_latency(self):
        with self.lock:
            if not self.recent_latencies:
                return 0.0
            return sum(self.recent_latencies) / len(self.recent_latencies)

    # ── 加密协议 real path 控制 ─────────────────────────────
    def is_crypto_enabled(self):
        with self.lock:
            return self.crypto_enabled

    def toggle_crypto_enabled(self):
        with self.lock:
            self.crypto_enabled = not self.crypto_enabled
            return self.crypto_enabled

    def is_response_encryption(self):
        with self.lock:
            return self.response_encryption

    def toggle_response_encryption(self):
        with self.lock:
            self.response_encryption = not self.response_encryption
            return self.response_encryption

    def request_crypto_reset(self):
        with self.lock:
            self.crypto_reset_pending = True

    def consume_crypto_reset(self):
        with self.lock:
            val = self.crypto_reset_pending
            self.crypto_reset_pending = False
            return val

    # ── 配钥授权（NFC 刷卡）控制 ────────────────────────────
    def request_pair_approve(self):
        with self.lock:
            self.pair_approve_pending = True

    def request_pair_reject(self):
        with self.lock:
            self.pair_reject_pending = True

    def consume_pair_requests(self):
        """取出一次待处理的批准/拒绝操作。"""
        with self.lock:
            approve, reject = self.pair_approve_pending, self.pair_reject_pending
            self.pair_approve_pending = False
            self.pair_reject_pending = False
            return approve, reject

    def set_pair_pending_desc(self, desc):
        with self.lock:
            self.pair_pending_desc = desc or ""

    def get_pair_pending_desc(self):
        with self.lock:
            return self.pair_pending_desc


def pack_telemetry(data):
    """
    将字典数据序列化为紧凑的二进制 SimPayload (53 bytes)
    """
    gear_byte = data['gear'].encode('ascii')
    doors = [
        int(data['door_open_fl']),
        int(data['door_open_fr']),
        int(data['door_open_rl']),
        int(data['door_open_rr']),
        int(data['door_open_trunk_front']),
        int(data['door_open_trunk_rear'])
    ]
    return struct.pack(
        PAYLOAD_FORMAT,
        data['speed_kmh'],
        data['motor_power_kw'],
        data['battery_level'],
        data['battery_range_km'],
        gear_byte,
        *doors,
        int(data['locked']),
        int(data['charging']),
        data['charge_power_kw'],
        data['inside_temp'],
        data['outside_temp'],
        data['tpms_fl'],
        data['tpms_fr'],
        data['tpms_rl'],
        data['tpms_rr']
    )


def generate_mock_frame(frame_counter):
    """
    根据时间轴帧数（0 ~ 1030 帧）生成仿真的车机遥测状态数据。
    """
    loop_frame = frame_counter % 1030
    
    data = {
        'speed_kmh': 0.0,
        'motor_power_kw': 0.0,
        'battery_level': 62,
        'battery_range_km': 248.0,
        'gear': 'P',
        'door_open_fl': False,
        'door_open_fr': False,
        'door_open_rl': False,
        'door_open_rr': False,
        'door_open_trunk_front': False,
        'door_open_trunk_rear': False,
        'locked': True,
        'charging': False,
        'charge_power_kw': 0.0,
        'inside_temp': 35.0,
        'outside_temp': 16.0,
        'tpms_fl': 2.3,
        'tpms_fr': 2.3,
        'tpms_rl': 2.3,
        'tpms_rr': 2.3,
        'script_phase_desc': "阶段 0: 待机/就绪"
    }

    # 阶段 0：解锁上车（0 ~ 100帧）
    if loop_frame < 100:
        data['locked'] = False
        data['door_open_fl'] = True
        data['door_open_rl'] = True
        data['tpms_fl'] = 2.4
        data['tpms_fr'] = 2.8
        data['tpms_rl'] = 3.3
        data['tpms_rr'] = 2.9
        data['script_phase_desc'] = "阶段 0: 解锁上车 (车门开启, 胎压异常)"

    # 阶段 1：准备起步（100 ~ 160帧）
    elif 100 <= loop_frame < 160:
        data['locked'] = False
        data['motor_power_kw'] = 1.5
        data['tpms_fl'] = 2.4
        data['tpms_fr'] = 2.8
        data['tpms_rl'] = 3.3
        data['tpms_rr'] = 2.9
        data['script_phase_desc'] = "阶段 1: 准备起步 (车门关闭, 电机微动)"

    # 阶段 2：挂D档起步（160 ~ 240帧）
    elif 240 > loop_frame >= 160:
        data['locked'] = False
        data['gear'] = 'D'
        if loop_frame < 200:
            data['speed_kmh'] = 0.0
            data['motor_power_kw'] = 0.0
        else:
            ratio = (loop_frame - 200) / 40.0
            data['speed_kmh'] = ratio * 30.0
            data['motor_power_kw'] = 12.0
        data['tpms_fl'] = 2.4
        data['tpms_fr'] = 2.8
        data['tpms_rl'] = 3.3
        data['tpms_rr'] = 2.9
        data['script_phase_desc'] = "阶段 2: 挂D挡起步加速中"

    # 阶段 3：强力超车（240 ~ 360帧）
    elif 360 > loop_frame >= 240:
        data['locked'] = False
        ratio = (loop_frame - 240) / 120.0
        data['gear'] = 'D'
        data['speed_kmh'] = 30.0 + ratio * 90.0
        data['motor_power_kw'] = 25.0 + ratio * 60.0
        data['tpms_fl'] = 2.4
        data['tpms_fr'] = 2.8 + ratio * 0.3
        data['tpms_rl'] = 3.3
        data['tpms_rr'] = 2.9 + ratio * 0.4
        data['inside_temp'] = 35.0 - ratio * 13.0
        data['script_phase_desc'] = "阶段 3: 强力超车 (120 km/h, 大功率输出)"

    # 阶段 4：能量回收重踩刹车（360 ~ 460帧）
    elif 460 > loop_frame >= 360:
        data['locked'] = False
        data['gear'] = 'D'
        if loop_frame < 410:
            ratio = (loop_frame - 360) / 50.0
            data['speed_kmh'] = 120.0 - ratio * 30.0
            data['motor_power_kw'] = -12.5
        else:
            ratio = (loop_frame - 410) / 50.0
            data['speed_kmh'] = 90.0 - ratio * 80.0
            data['motor_power_kw'] = -12.5 - ratio * 47.5
        data['tpms_fl'] = 2.4
        data['tpms_fr'] = 3.1
        data['tpms_rl'] = 3.3
        data['tpms_rr'] = 3.3
        data['script_phase_desc'] = "阶段 4: 强力能量回收制动"

    # 阶段 5：巡航模式 FSD (定速 80 ~ 85)（460 ~ 580帧）
    elif 580 > loop_frame >= 460:
        data['locked'] = False
        data['gear'] = '?'
        if loop_frame < 500:
            data['speed_kmh'] = 80.0
            data['motor_power_kw'] = 8.5
        elif loop_frame < 540:
            r = (loop_frame - 500) / 40.0
            data['speed_kmh'] = 80.0 + r * 5.0
            data['motor_power_kw'] = 12.0
        else:
            r = (loop_frame - 540) / 40.0
            data['speed_kmh'] = 85.0 - r * 5.0
            data['motor_power_kw'] = 4.0
        data['tpms_fl'] = 2.4
        data['tpms_fr'] = 3.1
        data['tpms_rl'] = 3.3
        data['tpms_rr'] = 3.3
        data['script_phase_desc'] = "阶段 5: FSD 自动巡航中"

    # 阶段 6：倒车挂R档（580 ~ 700帧）
    elif 700 > loop_frame >= 580:
        data['locked'] = False
        data['gear'] = 'R'
        if loop_frame < 620:
            data['speed_kmh'] = 0.0
            data['motor_power_kw'] = 0.0
        elif loop_frame < 660:
            r = (loop_frame - 620) / 40.0
            data['speed_kmh'] = r * 6.0
            data['motor_power_kw'] = r * 5.0
        else:
            r = (loop_frame - 660) / 40.0
            data['speed_kmh'] = 6.0 - r * 6.0
            data['motor_power_kw'] = 4.0 - r * 4.0
        ratio = (loop_frame - 580) / 120.0
        data['tpms_fl'] = 2.4
        data['tpms_fr'] = 3.1 - ratio * 0.3
        data['tpms_rl'] = 3.3
        data['tpms_rr'] = 3.3 - ratio * 0.4
        data['script_phase_desc'] = "阶段 6: 挂R挡倒车入库"

    # 阶段 7：开门（700 ~ 780帧）
    elif 780 > loop_frame >= 700:
        data['locked'] = False
        data['door_open_fl'] = True
        data['tpms_fl'] = 2.4
        data['tpms_fr'] = 2.8
        data['tpms_rl'] = 3.3
        data['tpms_rr'] = 2.9
        data['script_phase_desc'] = "阶段 7: 抵达目的地, 驾驶员下车"

    # 阶段 8：插枪充电中（780 ~ 1030帧）
    else:
        data['locked'] = True
        data['charging'] = True
        data['charge_power_kw'] = 7.2
        ratio = (loop_frame - 780) / 250.0
        data['battery_level'] = min(100, 99 + int(ratio * 2.0))
        data['battery_range_km'] = 248.0 + ratio * 12.0
        data['tpms_fl'] = 2.4
        data['tpms_fr'] = 2.8
        data['tpms_rl'] = 3.3
        data['tpms_rr'] = 2.9
        data['script_phase_desc'] = "阶段 8: 插枪充电中 (7.2 kW 慢充)"

    return data


# ─── 特斯拉深色主题 GUI 界面类 ────────────────────────────────
class TeslaSimulatorGUI:
    def __init__(self, root, state: TelemetryState, stop_event: threading.Event, log_queue: queue.Queue):
        self.root = root
        self.state = state
        self.stop_event = stop_event
        self.log_queue = log_queue
        
        self.is_syncing_ui = False
        
        # 配色定义 (特斯拉暗黑风)
        self.BG_MAIN = "#0F0F11"       # 窗口背景
        self.BG_CARD = "#18181B"       # 卡片容器背景
        self.BG_INNER = "#202024"      # 内部组件底色
        self.FG_TEXT = "#F4F4F5"       # 高亮文字
        self.FG_MUTED = "#A1A1AA"      # 次要灰色文字
        self.COLOR_RED = "#E82127"      # 特斯拉标志红
        self.COLOR_GREEN = "#10B981"    # 充电绿
        self.COLOR_BLUE = "#3B82F6"     # 自动模式蓝
        self.BG_BTN = "#27272A"         # 扁平按钮颜色
        self.BG_BTN_HOVER = "#3F3F46"   # 悬停颜色

        # 配置 root 窗口属性
        self.root.title("Tesla BLE TFT 仪表盘 - 仿真控制器 v2.0 稳定版")
        self.root.geometry("1120x820")
        self.root.minsize(1050, 760)
        self.root.config(bg=self.BG_MAIN)
        
        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(2, weight=1)

        # 配钥授权弹窗（等待刷卡时自动弹出）
        self._pair_dialog = None
        self._pair_dialog_desc = None
        self._pair_btn_approve = None
        self._pair_btn_reject = None
        self._pair_approve_clicked = False

        
        self.create_top_panel()
        self.create_emulation_bar()
        self.create_main_content()
        
        # 初始化 UI 的控件值
        self.sync_ui_from_data(self.state.get_data())

    def create_top_panel(self):
        """
        创建顶层控制栏：显示连接状态、VIN 配置和模式切换
        """
        top_frame = tk.Frame(self.root, bg=self.BG_CARD, height=60, bd=0, relief="flat")
        top_frame.grid(row=0, column=0, sticky="ew", padx=10, pady=(10, 5))
        
        top_frame.columnconfigure(2, weight=1)
        top_frame.columnconfigure(5, weight=1)
        
        # 标志 Title
        lbl_title = tk.Label(top_frame, text="TESLA SIMULATOR", font=("Segoe UI", 11, "bold"), fg=self.COLOR_RED, bg=self.BG_CARD)
        lbl_title.grid(row=0, column=0, padx=10, pady=15, sticky="w")
        
        # 蓝牙连接指示灯子 Frame
        status_sub = tk.Frame(top_frame, bg=self.BG_CARD)
        status_sub.grid(row=0, column=1, padx=5, sticky="w")
        
        self.status_canvas = tk.Canvas(status_sub, width=12, height=12, bg=self.BG_CARD, bd=0, highlightthickness=0)
        self.status_canvas.pack(side="left", padx=(5, 3))
        self.status_led = self.status_canvas.create_oval(2, 2, 10, 10, fill="#555555", width=0)
        
        self.lbl_status = tk.Label(status_sub, text="等待连接...", font=("Segoe UI", 9, "bold"), fg=self.FG_MUTED, bg=self.BG_CARD)
        self.lbl_status.pack(side="left")
        
        # 占位 Space
        lbl_spacer = tk.Label(top_frame, bg=self.BG_CARD)
        lbl_spacer.grid(row=0, column=2, sticky="ew")
        
        # VIN 配置组件组
        vin_frame = tk.Frame(top_frame, bg=self.BG_CARD)
        vin_frame.grid(row=0, column=3, padx=10, sticky="e")
        
        lbl_vin = tk.Label(vin_frame, text="VIN:", font=("Segoe UI", 9, "bold"), fg=self.FG_MUTED, bg=self.BG_CARD)
        lbl_vin.pack(side="left", padx=2)
        
        self.entry_vin = tk.Entry(
            vin_frame,
            font=("Consolas", 10),
            bg=self.BG_INNER,
            fg=self.FG_TEXT,
            insertbackground=self.FG_TEXT,
            bd=1,
            relief="flat",
            width=19
        )
        self.entry_vin.pack(side="left", padx=3)
        self.entry_vin.insert(0, self.state.get_vin())
        self.entry_vin.bind("<KeyRelease>", self.on_vin_key_release)
        
        # 实时计算广播名 Label
        self.lbl_ble_name = tk.Label(vin_frame, text="", font=("Consolas", 9), fg=self.COLOR_GREEN, bg=self.BG_CARD)
        self.lbl_ble_name.pack(side="left", padx=5)
        self.update_ble_name_preview()
        
        # 重启广播按钮
        self.btn_restart_ble = tk.Button(
            vin_frame,
            text="应用并重启广播",
            font=("Segoe UI", 9),
            bg=self.BG_BTN,
            fg=self.FG_TEXT,
            activebackground=self.BG_BTN_HOVER,
            activeforeground=self.FG_TEXT,
            bd=0,
            relief="flat",
            padx=10,
            pady=3,
            command=self.restart_ble_service
        )
        self.btn_restart_ble.pack(side="left", padx=5)
        self.btn_restart_ble.bind("<Enter>", lambda e: self.btn_restart_ble.config(bg=self.BG_BTN_HOVER))
        self.btn_restart_ble.bind("<Leave>", lambda e: self.btn_restart_ble.config(bg=self.BG_BTN))
        
        # 蓝牙广播总开关按钮
        self.btn_toggle_ble = tk.Button(
            vin_frame,
            text="BLE广播: ON",
            font=("Segoe UI", 9, "bold"),
            bg=self.COLOR_GREEN,
            fg=self.FG_TEXT,
            activebackground=self.COLOR_GREEN,
            activeforeground=self.FG_TEXT,
            bd=0,
            relief="flat",
            padx=10,
            pady=3,
            command=self.toggle_ble_switch
        )
        self.btn_toggle_ble.pack(side="left", padx=5)
        
        # 模式切换大 Toggle 按钮 (依据当前默认的模式决定初始状态)
        self.btn_mode = tk.Button(
            top_frame,
            text="自动剧本模式 (PLAYING)" if self.state.is_script_mode() else "手动干预模式 (MANUAL)",
            font=("Segoe UI", 10, "bold"),
            bg=self.get_current_mode_color(),
            fg=self.FG_TEXT,
            activebackground=self.get_current_mode_color(),
            activeforeground=self.FG_TEXT,
            bd=0,
            relief="flat",
            padx=15,
            pady=6,
            command=self.toggle_simulation_mode
        )
        self.btn_mode.grid(row=0, column=4, padx=10, sticky="e")
        self.btn_mode.bind("<Enter>", lambda e: self.btn_mode.config(bg=self.adjust_color_brightness(self.btn_mode['bg'], 1.2)))
        self.btn_mode.bind("<Leave>", lambda e: self.btn_mode.config(bg=self.get_current_mode_color()))

    def update_ble_name_preview(self):
        vin = self.entry_vin.get().strip()
        if len(vin) == 17:
            try:
                sha1 = hashlib.sha1(vin.encode('utf-8')).hexdigest()
                ble_name = "S" + sha1[:16] + "C"
                self.lbl_ble_name.config(text=f"({ble_name})", fg=self.COLOR_GREEN)
            except Exception:
                self.lbl_ble_name.config(text="(计算错误)", fg=self.COLOR_RED)
        else:
            self.lbl_ble_name.config(text="(VIN必须17位)", fg=self.COLOR_RED)

    def on_vin_key_release(self, event):
        self.update_ble_name_preview()

    def restart_ble_service(self):
        new_vin = self.entry_vin.get().strip()
        if not new_vin or len(new_vin) != 17:
            messagebox.showerror("错误", "特斯拉 VIN 必须为 17 位字符！\n示例: LRWYGCFJ8RC571213")
            return
        
        self.state.set_vin(new_vin)
        self.log_queue.put(f"[System] 正在请求重启蓝牙广播，新 VIN: {new_vin}")
        self.btn_restart_ble.config(state="disabled", text="重启中...")
        self.state.set_restart(True)

    def toggle_ble_switch(self):
        is_on = self.state.toggle_ble_enabled()
        self.update_ble_toggle_button_ui(is_on)

    def update_ble_toggle_button_ui(self, is_on):
        if is_on:
            self.btn_toggle_ble.config(
                text="BLE广播: ON",
                bg=self.COLOR_GREEN,
                activebackground=self.COLOR_GREEN
            )
        else:
            self.btn_toggle_ble.config(
                text="BLE广播: OFF",
                bg=self.COLOR_RED,
                activebackground=self.COLOR_RED
            )

    def create_emulation_bar(self):
        """
        创建真车 BLE 通讯拟真控制栏（问答模式切换、延迟/丢包控制、故障注入）
        """
        # 外层容器：控制栏 + 配钥授权横幅各占一行。
        # 控制栏控件很多，宽度超出窗口时后 pack 的控件会被挤掉不可见，
        # 所以「轻点钥匙卡」必须放在独立一行保证永远可见。
        wrap = tk.Frame(self.root, bg=self.BG_MAIN)
        wrap.grid(row=1, column=0, sticky="ew", padx=10, pady=(0, 5))

        bar_frame = tk.Frame(wrap, bg=self.BG_CARD, height=42, bd=0, relief="flat")
        bar_frame.pack(fill="x")
        
        # 1. 标题与模式 Toggle
        lbl_tag = tk.Label(bar_frame, text="真车BLE拟真:", font=("Segoe UI", 9, "bold"), fg=self.COLOR_BLUE, bg=self.BG_CARD)
        lbl_tag.pack(side="left", padx=(10, 5), pady=6)
        
        mode_text = "问答响应模式 (REALISTIC)" if self.state.get_comm_mode() == "REALISTIC" else "理想主动推送 (IDEAL)"
        self.btn_comm_mode = tk.Button(
            bar_frame,
            text=mode_text,
            font=("Segoe UI", 9, "bold"),
            bg=self.COLOR_BLUE if self.state.get_comm_mode() == "REALISTIC" else self.BG_BTN,
            fg=self.FG_TEXT,
            activebackground=self.COLOR_BLUE,
            activeforeground=self.FG_TEXT,
            bd=0,
            relief="flat",
            padx=8,
            pady=2,
            command=self.toggle_comm_mode_switch
        )
        self.btn_comm_mode.pack(side="left", padx=5)

        # 1.5 加密协议通道开关（ON = 真实握手/加解密；OFF = 53B 旁路）
        crypto_on = self.state.is_crypto_enabled()
        self.btn_crypto = tk.Button(
            bar_frame,
            text="加密协议: ON" if crypto_on else "加密协议: OFF (53B旁路)",
            font=("Segoe UI", 9, "bold"),
            bg=self.COLOR_GREEN if crypto_on else self.BG_BTN,
            fg=self.FG_TEXT,
            activebackground=self.COLOR_GREEN if crypto_on else self.BG_BTN,
            activeforeground=self.FG_TEXT,
            bd=0,
            relief="flat",
            padx=8,
            pady=2,
            command=self.toggle_crypto_switch
        )
        self.btn_crypto.pack(side="left", padx=5)

        # 2. 实时链路监控 Label
        self.lbl_emulation_monitor = tk.Label(
            bar_frame,
            text="[REAL] 队列: 0/32 | 延时: ~0ms",
            font=("Consolas", 9, "bold"),
            fg=self.COLOR_GREEN,
            bg=self.BG_CARD
        )
        self.lbl_emulation_monitor.pack(side="left", padx=12)

        # 3. 延时倍率 Slider
        lbl_delay = tk.Label(bar_frame, text="延时倍率:", font=("Segoe UI", 8, "bold"), fg=self.FG_MUTED, bg=self.BG_CARD)
        lbl_delay.pack(side="left", padx=(10, 2))
        
        self.scale_delay_mult = tk.Scale(
            bar_frame,
            from_=0.2,
            to=3.0,
            resolution=0.1,
            orient="horizontal",
            showvalue=True,
            font=("Segoe UI", 8),
            bg=self.BG_CARD,
            fg=self.FG_TEXT,
            highlightthickness=0,
            length=90,
            command=self.on_delay_mult_change
        )
        self.scale_delay_mult.set(self.state.get_delay_multiplier())
        self.scale_delay_mult.pack(side="left", padx=2)

        # 4. 模拟丢包率 Slider
        lbl_loss = tk.Label(bar_frame, text="丢包率:", font=("Segoe UI", 8, "bold"), fg=self.FG_MUTED, bg=self.BG_CARD)
        lbl_loss.pack(side="left", padx=(10, 2))
        
        self.scale_loss = tk.Scale(
            bar_frame,
            from_=0,
            to=20,
            resolution=1,
            orient="horizontal",
            showvalue=True,
            font=("Segoe UI", 8),
            bg=self.BG_CARD,
            fg=self.FG_TEXT,
            highlightthickness=0,
            length=80,
            command=self.on_loss_rate_change
        )
        self.scale_loss.set(int(self.state.get_loss_rate() * 100))
        self.scale_loss.pack(side="left", padx=2)

        # 5. 故障注入 Button
        btn_fault = tk.Button(
            bar_frame,
            text="⚡ 注入 5s 信号闪断",
            font=("Segoe UI", 9, "bold"),
            bg=self.COLOR_RED,
            fg=self.FG_TEXT,
            activebackground=self.COLOR_RED,
            activeforeground=self.FG_TEXT,
            bd=0,
            relief="flat",
            padx=10,
            pady=2,
            command=self.inject_fault_blackout
        )
        btn_fault.pack(side="right", padx=10)

        # 6. 使会话失效（模拟 Infotainment 重启，客户端应自动重新握手）
        btn_reset = tk.Button(
            bar_frame,
            text="♻ 使会话失效",
            font=("Segoe UI", 9, "bold"),
            bg=self.BG_BTN,
            fg=self.FG_TEXT,
            activebackground=self.BG_BTN_HOVER,
            activeforeground=self.FG_TEXT,
            bd=0,
            relief="flat",
            padx=8,
            pady=2,
            command=self.invalidate_crypto_session
        )
        btn_reset.pack(side="right", padx=(0, 6))

        # 7. 响应加密开关（ON = 2024.38+ 加密响应；OFF = 老固件明文响应）
        resp_enc = self.state.is_response_encryption()
        self.btn_resp_enc = tk.Button(
            bar_frame,
            text="响应加密: 2024.38+" if resp_enc else "响应加密: 老固件明文",
            font=("Segoe UI", 9, "bold"),
            bg=self.COLOR_BLUE if resp_enc else self.BG_BTN,
            fg=self.FG_TEXT,
            activebackground=self.COLOR_BLUE if resp_enc else self.BG_BTN,
            activeforeground=self.FG_TEXT,
            bd=0,
            relief="flat",
            padx=8,
            pady=2,
            command=self.toggle_response_encryption
        )
        self.btn_resp_enc.pack(side="right", padx=(0, 6))

        # 8. 配钥授权横幅（独立一行，始终可见；模拟中控台「轻点 NFC 钥匙卡」）
        self.pair_banner = tk.Frame(wrap, bg=self.BG_INNER, bd=0, relief="flat")
        self.pair_banner.pack(fill="x", pady=(4, 0))

        self.lbl_pair_banner = tk.Label(
            self.pair_banner,
            text="🔑 配钥授权：当前没有待授权的 add-key-request",
            font=("Segoe UI", 9, "bold"),
            fg=self.FG_MUTED,
            bg=self.BG_INNER,
            anchor="w"
        )
        self.lbl_pair_banner.pack(side="left", padx=10, pady=4)

        self.btn_pair_reject = tk.Button(
            self.pair_banner,
            text="拒绝",
            font=("Segoe UI", 9, "bold"),
            bg=self.BG_BTN,
            fg=self.FG_TEXT,
            activebackground=self.BG_BTN_HOVER,
            activeforeground=self.FG_TEXT,
            bd=0,
            relief="flat",
            padx=10,
            pady=2,
            state="disabled",
            command=self._pair_dialog_reject
        )
        self.btn_pair_reject.pack(side="right", padx=(0, 10), pady=3)

        self.btn_pair_approve = tk.Button(
            self.pair_banner,
            text="🔑 轻点钥匙卡（批准）",
            font=("Segoe UI", 9, "bold"),
            bg=self.BG_BTN,
            fg=self.FG_TEXT,
            activebackground=self.COLOR_GREEN,
            activeforeground=self.FG_TEXT,
            bd=0,
            relief="flat",
            padx=10,
            pady=2,
            state="disabled",
            command=self.tap_nfc_card
        )
        self.btn_pair_approve.pack(side="right", padx=(0, 6), pady=3)

    def tap_nfc_card(self):
        """模拟车主在中控台轻点钥匙卡（授权 add-key-request）。"""
        self._pair_approve_clicked = True
        self.state.request_pair_approve()
        if getattr(self, "btn_pair_approve", None) is not None:
            self.btn_pair_approve.config(state="disabled", text="已刷卡，等待车辆确认…")
        self.log_queue.put("[GUI] 🔑 已请求刷卡授权，等待后台配钥状态机处理")

    # ─── 配钥授权弹窗 ──────────────────────────────────────────
    def _pair_dialog_alive(self):
        dlg = self._pair_dialog
        if dlg is None:
            return False
        try:
            return bool(dlg.winfo_exists())
        except Exception:
            return False

    def _sync_pair_dialog(self, pending):
        """有待授权请求时弹出窗口；请求结束（批准/拒绝/超时）后自动关闭。"""
        if pending and not self._pair_dialog_alive():
            self._show_pair_dialog(pending)
        elif not pending and self._pair_dialog_alive():
            self._close_pair_dialog()

    def _show_pair_dialog(self, desc):
        try:
            dlg = tk.Toplevel(self.root)
            dlg.title("🔑 BLE 配钥授权")
            dlg.configure(bg=self.BG_MAIN)
            dlg.resizable(False, False)
            dlg.transient(self.root)

            self.root.update_idletasks()
            width, height = 480, 250
            x = self.root.winfo_rootx() + max(0, (self.root.winfo_width() - width) // 2)
            y = self.root.winfo_rooty() + max(0, (self.root.winfo_height() - height) // 3)
            dlg.geometry(f"{width}x{height}+{x}+{y}")

            body = tk.Frame(dlg, bg=self.BG_CARD)
            body.pack(fill="both", expand=True, padx=12, pady=12)

            tk.Label(
                body,
                text="车辆请求登记新钥匙",
                font=("Segoe UI", 14, "bold"),
                fg=self.COLOR_RED,
                bg=self.BG_CARD,
            ).pack(anchor="w")

            tk.Label(
                body,
                text=desc,
                font=("Consolas", 10),
                fg=self.FG_TEXT,
                bg=self.BG_CARD,
                wraplength=440,
                justify="left",
            ).pack(anchor="w", pady=(8, 4))

            tk.Label(
                body,
                text="请在车辆中控台轻点 NFC 钥匙卡；也可以点击下方按钮模拟刷卡。\n"
                     "授权成功后该公钥会写入车端白名单，ESP32 会自动重新握手。",
                font=("Segoe UI", 9),
                fg=self.FG_MUTED,
                bg=self.BG_CARD,
                wraplength=440,
                justify="left",
            ).pack(anchor="w", pady=(0, 8))

            btns = tk.Frame(body, bg=self.BG_CARD)
            btns.pack(fill="x", side="bottom", pady=(6, 0))

            self._pair_btn_approve = tk.Button(
                btns,
                text="🔑 轻点钥匙卡（批准）",
                font=("Segoe UI", 10, "bold"),
                bg=self.COLOR_GREEN,
                fg=self.FG_TEXT,
                activebackground=self.COLOR_GREEN,
                activeforeground=self.FG_TEXT,
                bd=0,
                relief="flat",
                padx=12,
                pady=6,
                command=self._pair_dialog_approve,
            )
            self._pair_btn_approve.pack(side="left", expand=True, fill="x", padx=(0, 6))

            self._pair_btn_reject = tk.Button(
                btns,
                text="拒绝",
                font=("Segoe UI", 10, "bold"),
                bg=self.BG_BTN,
                fg=self.FG_TEXT,
                activebackground=self.BG_BTN_HOVER,
                activeforeground=self.FG_TEXT,
                bd=0,
                relief="flat",
                padx=12,
                pady=6,
                command=self._pair_dialog_reject,
            )
            self._pair_btn_reject.pack(side="left", expand=True, fill="x")

            dlg.protocol("WM_DELETE_WINDOW", self._pair_dialog_reject)
            self._pair_dialog = dlg
            self._pair_dialog_desc = desc
            dlg.lift()
            dlg.attributes("-topmost", True)
            dlg.after(800, lambda: self._drop_topmost(dlg))
            self.log_queue.put("[GUI] 🔑 已弹出配钥授权窗口，请刷卡或点击「批准」")
        except Exception as exc:
            self._pair_dialog = None
            self._pair_dialog_desc = None
            msg = f"[GUI] 配钥授权弹窗创建失败: {exc}"
            self.log_queue.put(msg)
            print(msg)

    @staticmethod
    def _drop_topmost(dlg):
        try:
            dlg.attributes("-topmost", False)
        except Exception:
            pass

    def _close_pair_dialog(self):
        dlg = self._pair_dialog
        self._pair_dialog = None
        self._pair_dialog_desc = None
        self._pair_btn_approve = None
        self._pair_btn_reject = None
        if dlg is not None:
            try:
                dlg.destroy()
            except Exception:
                pass

    def _pair_dialog_approve(self):
        self._pair_approve_clicked = True
        self.state.request_pair_approve()
        self.log_queue.put("[GUI] 🔑 已点击「轻点钥匙卡」，等待车辆确认")
        if getattr(self, "btn_pair_approve", None) is not None:
            try:
                self.btn_pair_approve.config(state="disabled", text="已刷卡，等待车辆确认…")
            except Exception:
                pass
        for btn in (self._pair_btn_approve, self._pair_btn_reject):
            if btn is not None:
                try:
                    btn.config(state="disabled")
                except Exception:
                    pass
        if self._pair_btn_approve is not None:
            try:
                self._pair_btn_approve.config(text="已刷卡，等待车辆确认…")
            except Exception:
                pass

    def _pair_dialog_reject(self):
        self.state.request_pair_reject()
        self.log_queue.put("[GUI] 已请求拒绝本次配钥")
        self._close_pair_dialog()

    def toggle_crypto_switch(self):
        enabled = self.state.toggle_crypto_enabled()
        if enabled:
            self.btn_crypto.config(
                text="加密协议: ON",
                bg=self.COLOR_GREEN,
                activebackground=self.COLOR_GREEN
            )
            self.log_queue.put("[System] 已启用真实加密协议通道（ESP32 需先握手，遥测改为问答驱动）")
        else:
            self.btn_crypto.config(
                text="加密协议: OFF (53B旁路)",
                bg=self.BG_BTN,
                activebackground=self.BG_BTN
            )
            self.log_queue.put("[System] 已切回 53B 旁路模式（ESP32 直接接收裸 SimPayload）")

    def toggle_response_encryption(self):
        enabled = self.state.toggle_response_encryption()
        if enabled:
            self.btn_resp_enc.config(
                text="响应加密: 2024.38+",
                bg=self.COLOR_BLUE,
                activebackground=self.COLOR_BLUE
            )
            self.log_queue.put("[System] 响应加密已开启（模拟 2024.38+ 固件）")
        else:
            self.btn_resp_enc.config(
                text="响应加密: 老固件明文",
                bg=self.BG_BTN,
                activebackground=self.BG_BTN
            )
            self.log_queue.put("[System] 响应加密已关闭（模拟老固件，响应保持明文）")

    def invalidate_crypto_session(self):
        self.state.request_crypto_reset()
        self.log_queue.put("[System] 已请求使加密会话失效（模拟车机重启）")

    def toggle_comm_mode_switch(self):
        new_mode = self.state.toggle_comm_mode()
        if new_mode == "REALISTIC":
            self.btn_comm_mode.config(
                text="问答响应模式 (REALISTIC)",
                bg=self.COLOR_BLUE,
                activebackground=self.COLOR_BLUE
            )
            self.log_queue.put("[System] 通讯模式切至: 真实车机问答响应模式 (150~300ms 真实行车延时与队列排队)")
        else:
            self.btn_comm_mode.config(
                text="理想主动推送 (IDEAL)",
                bg=self.BG_BTN,
                activebackground=self.BG_BTN
            )
            self.log_queue.put("[System] 通讯模式切至: 理想 20Hz 主动推送模式")

    def on_delay_mult_change(self, val):
        self.state.set_delay_multiplier(float(val))

    def on_loss_rate_change(self, val):
        self.state.set_loss_rate(float(val) / 100.0)

    def inject_fault_blackout(self):
        self.state.inject_fault(5.0)
        self.log_queue.put("[System] ⚠️ 触发故障注入：模拟 BLE 无线信号闪断 5 秒！")

    def create_main_content(self):
        """
        创建中部核心网格布局
        """
        main_frame = tk.Frame(self.root, bg=self.BG_MAIN)
        main_frame.grid(row=2, column=0, sticky="nsew", padx=10, pady=5)
        
        main_frame.columnconfigure(0, weight=1)
        main_frame.columnconfigure(1, weight=1)
        main_frame.rowconfigure(0, weight=1)
        main_frame.rowconfigure(1, weight=1)
        
        # 1. 驾控参数控制卡片 (左上)
        self.card_driving = self.create_card(main_frame, "驾控核心参数 CONTROLS", 0, 0)
        self.setup_driving_sliders()
        
        # 2. 车身与充电卡片 (左下)
        self.card_body = self.create_card(main_frame, "车身组件 & 充电状态 BODY & CHARGING", 1, 0)
        self.setup_body_controls()
        
        # 3. 环境温控与胎压卡片 (右上)
        self.card_env = self.create_card(main_frame, "环境温度 & 实时胎压 TPMS", 0, 1)
        self.setup_env_controls()
        
        # 4. 日志终端控制卡片 (右下)
        self.card_logs = self.create_card(main_frame, "实时数据传输日志 TERMINAL LOGS", 1, 1)
        self.setup_log_terminal()

    def create_card(self, parent, title, row, col):
        """
        卡片样式工厂函数
        """
        card = tk.LabelFrame(
            parent,
            text=f"  {title}  ",
            font=("Segoe UI", 10, "bold"),
            fg=self.FG_MUTED,
            bg=self.BG_CARD,
            bd=1,
            relief="solid",
            highlightthickness=0
        )
        card.config(highlightbackground="#2D2D2D")
        card.grid(row=row, column=col, sticky="nsew", padx=5, pady=5)
        card.columnconfigure(0, weight=1)
        return card

    def setup_driving_sliders(self):
        """
        构建驾控核心参数滑块
        """
        container = tk.Frame(self.card_driving, bg=self.BG_CARD)
        container.pack(fill="both", expand=True, padx=15, pady=10)
        container.columnconfigure(1, weight=1)
        
        self.slider_labels = {}
        self.sliders = {}
        
        slider_configs = [
            ("speed_kmh", "当前时速 (km/h)", 0.0, 200.0, 0.5, "%.1f km/h"),
            ("motor_power_kw", "电机输出功率 (kW)", -100.0, 300.0, 1.0, "%d kW"),
            ("battery_level", "电池电量 SOC (%)", 0.0, 100.0, 1.0, "%d %%"),
            ("battery_range_km", "估计续航里程 (km)", 0.0, 600.0, 1.0, "%d km")
        ]
        
        for idx, (name, label_text, min_val, max_val, resolution, fmt) in enumerate(slider_configs):
            lbl_desc = tk.Label(container, text=label_text, font=("Segoe UI", 10), fg=self.FG_MUTED, bg=self.BG_CARD)
            lbl_desc.grid(row=idx*2, column=0, sticky="w", pady=(8, 2))
            
            lbl_val = tk.Label(container, text="", font=("Segoe UI", 11, "bold"), fg=self.FG_TEXT, bg=self.BG_CARD)
            lbl_val.grid(row=idx*2, column=1, sticky="e", pady=(8, 2))
            self.slider_labels[name] = (lbl_val, fmt)
            
            scale = tk.Scale(
                container,
                from_=min_val,
                to=max_val,
                resolution=resolution,
                orient=tk.HORIZONTAL,
                bg=self.BG_CARD,
                fg=self.FG_TEXT,
                activebackground=self.COLOR_RED,
                troughcolor=self.BG_INNER,
                highlightthickness=0,
                bd=0,
                showvalue=0,
                command=lambda val, n=name: self.on_slider_change(n, val)
            )
            scale.grid(row=idx*2+1, column=0, columnspan=2, sticky="ew", pady=(0, 10))
            self.sliders[name] = scale

    def setup_body_controls(self):
        """
        构建车身部件及充电状态控制按钮
        """
        container = tk.Frame(self.card_body, bg=self.BG_CARD)
        container.pack(fill="both", expand=True, padx=15, pady=10)
        
        # 1. 挡位控制组
        gear_frame = tk.Frame(container, bg=self.BG_CARD)
        gear_frame.pack(fill="x", pady=(5, 10))
        
        lbl_gear = tk.Label(gear_frame, text="行驶档位 GEAR", font=("Segoe UI", 9, "bold"), fg=self.FG_MUTED, bg=self.BG_CARD)
        lbl_gear.pack(side="left", padx=(0, 15))
        
        self.gear_buttons = {}
        # gear_map: 按钮显示名称 -> 实际发送给 ESP32 的字符
        # SNA (Signal Not Available) 对应 ESP32 gear_from_shift_state 中的 '?' 字符，触发 AP/巡航界面
        gear_display_map = [
            ('P', 'P'),
            ('R', 'R'),
            ('N', 'N'),
            ('D', 'D'),
            ('SNA', '?'),  # 辅助驾驶档/信号未指定，蓝色高亮
        ]
        for display_label, char_val in gear_display_map:
            is_sna = (char_val == '?')
            btn = tk.Button(
                gear_frame,
                text=display_label,
                font=("Segoe UI", 10, "bold"),
                width=4 if not is_sna else 5,
                bg=self.BG_BTN,
                fg=self.FG_TEXT,
                activebackground=self.COLOR_BLUE if is_sna else self.COLOR_RED,
                activeforeground=self.FG_TEXT,
                bd=0,
                relief="flat",
                command=lambda cv=char_val: self.set_gear_state(cv)
            )
            btn.pack(side="left", padx=5)
            btn.bind("<Enter>", lambda e, b=btn: self.on_btn_hover_enter(b))
            btn.bind("<Leave>", lambda e, b=btn, cv=char_val: self.on_btn_hover_leave(b, 'gear', cv))
            self.gear_buttons[char_val] = btn
            
        # 2. 中控锁与充电状态 Toggle 按钮
        toggle_frame = tk.Frame(container, bg=self.BG_CARD)
        toggle_frame.pack(fill="x", pady=5)
        
        self.btn_lock = self.create_toggle_btn(toggle_frame, "中控锁定 [锁]", self.toggle_lock_state)
        self.btn_lock.pack(side="left", padx=(0, 10), expand=True, fill="x")
        
        self.btn_charge_plug = self.create_toggle_btn(toggle_frame, "充电枪插拔 [拔]", self.toggle_charge_plug)
        self.btn_charge_plug.pack(side="left", padx=10, expand=True, fill="x")
        
        # 3. 充电功率滑块 (依附于充电状态)
        charge_slider_frame = tk.Frame(container, bg=self.BG_CARD)
        charge_slider_frame.pack(fill="x", pady=(10, 5))
        charge_slider_frame.columnconfigure(1, weight=1)
        
        lbl_c_desc = tk.Label(charge_slider_frame, text="直流充电功率 (kW)", font=("Segoe UI", 10), fg=self.FG_MUTED, bg=self.BG_CARD)
        lbl_c_desc.grid(row=0, column=0, sticky="w")
        
        self.lbl_charge_val = tk.Label(charge_slider_frame, text="0 kW", font=("Segoe UI", 11, "bold"), fg=self.COLOR_GREEN, bg=self.BG_CARD)
        self.lbl_charge_val.grid(row=0, column=1, sticky="e")
        
        self.scale_charge_power = tk.Scale(
            charge_slider_frame,
            from_=0.0,
            to=250.0,
            resolution=1.0,
            orient=tk.HORIZONTAL,
            bg=self.BG_CARD,
            fg=self.FG_TEXT,
            activebackground=self.COLOR_GREEN,
            troughcolor=self.BG_INNER,
            highlightthickness=0,
            bd=0,
            showvalue=0,
            command=self.on_charge_power_change
        )
        self.scale_charge_power.grid(row=1, column=0, columnspan=2, sticky="ew", pady=(2, 5))
        
        # 4. 车身六门状态控制 (2x3 的按钮矩阵)
        door_label_frame = tk.Frame(container, bg=self.BG_CARD)
        door_label_frame.pack(fill="x", pady=(10, 2))
        lbl_door_title = tk.Label(door_label_frame, text="车身各门控制 DOORS (点击开启/关闭)", font=("Segoe UI", 9, "bold"), fg=self.FG_MUTED, bg=self.BG_CARD)
        lbl_door_title.pack(side="left")
        
        door_btn_frame = tk.Frame(container, bg=self.BG_CARD)
        door_btn_frame.pack(fill="both", expand=True, pady=5)
        door_btn_frame.columnconfigure(0, weight=1)
        door_btn_frame.columnconfigure(1, weight=1)
        door_btn_frame.columnconfigure(2, weight=1)
        
        self.door_buttons = {}
        doors_config = [
            ("door_open_fl", "左前门", 0, 0),
            ("door_open_fr", "右前门", 0, 1),
            ("door_open_rl", "左后门", 1, 0),
            ("door_open_rr", "右后门", 1, 1),
            ("door_open_trunk_front", "前备箱", 0, 2),
            ("door_open_trunk_rear", "后备箱", 1, 2)
        ]
        
        for key, name, r, c in doors_config:
            btn = tk.Button(
                door_btn_frame,
                text=f"{name} [关]",
                font=("Segoe UI", 9),
                bg=self.BG_BTN,
                fg=self.FG_TEXT,
                activebackground=self.COLOR_RED,
                activeforeground=self.FG_TEXT,
                bd=0,
                relief="flat",
                pady=6,
                command=lambda k=key: self.toggle_door_state(k)
            )
            btn.grid(row=r, column=c, padx=3, pady=3, sticky="ew")
            btn.bind("<Enter>", lambda e, b=btn: self.on_btn_hover_enter(b))
            btn.bind("<Leave>", lambda e, b=btn, k=key: self.on_btn_hover_leave(b, 'door', k))
            self.door_buttons[key] = btn

    def setup_env_controls(self):
        """
        构建环境温控与胎压卡片控件
        """
        container = tk.Frame(self.card_env, bg=self.BG_CARD)
        container.pack(fill="both", expand=True, padx=15, pady=10)
        container.columnconfigure(1, weight=1)
        
        # 1. 温度滑块
        self.env_labels = {}
        self.env_sliders = {}
        
        env_config = [
            ("inside_temp", "车内温度 (℃)", -10.0, 50.0, 0.5, "%.1f ℃"),
            ("outside_temp", "车外温度 (℃)", -10.0, 50.0, 0.5, "%.1f ℃")
        ]
        
        for idx, (name, label_text, min_val, max_val, resolution, fmt) in enumerate(env_config):
            lbl_desc = tk.Label(container, text=label_text, font=("Segoe UI", 10), fg=self.FG_MUTED, bg=self.BG_CARD)
            lbl_desc.grid(row=idx*2, column=0, sticky="w", pady=(5, 2))
            
            lbl_val = tk.Label(container, text="", font=("Segoe UI", 11, "bold"), fg=self.FG_TEXT, bg=self.BG_CARD)
            lbl_val.grid(row=idx*2, column=1, sticky="e", pady=(5, 2))
            self.env_labels[name] = (lbl_val, fmt)
            
            scale = tk.Scale(
                container,
                from_=min_val,
                to=max_val,
                resolution=resolution,
                orient=tk.HORIZONTAL,
                bg=self.BG_CARD,
                fg=self.FG_TEXT,
                activebackground=self.COLOR_RED,
                troughcolor=self.BG_INNER,
                highlightthickness=0,
                bd=0,
                showvalue=0,
                command=lambda val, n=name: self.on_env_slider_change(n, val)
            )
            scale.grid(row=idx*2+1, column=0, columnspan=2, sticky="ew", pady=(0, 8))
            self.env_sliders[name] = scale
            
        # 2. 胎压滑块组
        tpms_title_frame = tk.Frame(container, bg=self.BG_CARD)
        tpms_title_frame.grid(row=4, column=0, columnspan=2, sticky="w", pady=(15, 2))
        lbl_tpms_title = tk.Label(tpms_title_frame, text="四轮实时胎压 (Bar, 正常范围 2.6 - 3.1)", font=("Segoe UI", 9, "bold"), fg=self.FG_MUTED, bg=self.BG_CARD)
        lbl_tpms_title.pack(side="left")
        
        tpms_box = tk.Frame(container, bg=self.BG_CARD)
        tpms_box.grid(row=5, column=0, columnspan=2, sticky="nsew", pady=5)
        tpms_box.columnconfigure(0, weight=1)
        tpms_box.columnconfigure(1, weight=1)
        
        self.tpms_labels = {}
        self.tpms_sliders = {}
        
        tpms_config = [
            ("tpms_fl", "左前胎 (FL)", 0, 0),
            ("tpms_fr", "右前胎 (FR)", 0, 1),
            ("tpms_rl", "左后胎 (RL)", 1, 0),
            ("tpms_rr", "右后胎 (RR)", 1, 1)
        ]
        
        for key, name, r, c in tpms_config:
            cell = tk.Frame(tpms_box, bg=self.BG_CARD, padx=5, pady=2)
            cell.grid(row=r, column=c, sticky="ew")
            cell.columnconfigure(1, weight=1)
            
            lbl_n = tk.Label(cell, text=name, font=("Segoe UI", 9), fg=self.FG_MUTED, bg=self.BG_CARD)
            lbl_n.grid(row=0, column=0, sticky="w")
            
            lbl_v = tk.Label(cell, text="", font=("Segoe UI", 9, "bold"), fg=self.FG_TEXT, bg=self.BG_CARD)
            lbl_v.grid(row=0, column=1, sticky="e")
            self.tpms_labels[key] = lbl_v
            
            scale = tk.Scale(
                cell,
                from_=1.5,
                to=4.0,
                resolution=0.1,
                orient=tk.HORIZONTAL,
                bg=self.BG_CARD,
                fg=self.FG_TEXT,
                activebackground=self.COLOR_RED,
                troughcolor=self.BG_INNER,
                highlightthickness=0,
                bd=0,
                showvalue=0,
                command=lambda val, k=key: self.on_tpms_change(k, val)
            )
            scale.grid(row=1, column=0, columnspan=2, sticky="ew", pady=(0, 4))
            self.tpms_sliders[key] = scale

    def setup_log_terminal(self):
        """
        构建数据包实时传输日志控制台
        """
        container = tk.Frame(self.card_logs, bg=self.BG_CARD)
        container.pack(fill="both", expand=True, padx=15, pady=10)
        
        self.txt_log = tk.Text(
            container,
            bg="#08080A",
            fg="#10B981",
            font=("Consolas", 9),
            bd=0,
            highlightthickness=1,
            highlightbackground="#2D2D2D",
            padx=10,
            pady=10,
            wrap="word",
            state="disabled"
        )
        self.txt_log.pack(side="left", fill="both", expand=True)
        
        scrollbar = tk.Scrollbar(container, command=self.txt_log.yview, bg=self.BG_CARD, bd=0, highlightthickness=0)
        scrollbar.pack(side="right", fill="y")
        self.txt_log.config(yscrollcommand=scrollbar.set)

    def create_toggle_btn(self, parent, text, command):
        """
        扁平化 Toggle 按钮模板
        """
        return tk.Button(
            parent,
            text=text,
            font=("Segoe UI", 9, "bold"),
            bg=self.BG_BTN,
            fg=self.FG_TEXT,
            activebackground=self.BG_BTN_HOVER,
            activeforeground=self.FG_TEXT,
            bd=0,
            relief="flat",
            pady=8,
            command=command
        )

    # ─── UI 交互事件回调 ──────────────────────────────────────────
    def toggle_simulation_mode(self):
        """
        切换剧本自动演进/手动干预模式
        """
        current_mode = self.state.is_script_mode()
        new_mode = not current_mode
        self.state.set_script_mode(new_mode)
        
        if new_mode:
            self.state.reset_frame()
            self.btn_mode.config(text="自动剧本模式 (PLAYING)", bg=self.COLOR_BLUE)
            self.log_queue.put("[System] 切换至：自动剧本模式。手动输入已停用。")
        else:
            self.btn_mode.config(text="手动干预模式 (MANUAL)", bg=self.COLOR_RED)
            self.log_queue.put("[System] 切换至：手动干预模式。现在可通过控件调整车辆状态。")
        
        self.update_controls_interactive_state()

    def update_controls_interactive_state(self):
        """
        当切换模式时，在手动模式下启用控件，在自动模式下禁用控件（只读展示）
        """
        is_manual = not self.state.is_script_mode()
        state_str = "normal" if is_manual else "disabled"
        
        for s in self.sliders.values():
            s.config(state=state_str)
            
        for b in self.gear_buttons.values():
            b.config(state=state_str)
            
        for b in self.door_buttons.values():
            b.config(state=state_str)
            
        self.btn_lock.config(state=state_str)
        self.btn_charge_plug.config(state=state_str)
        
        if is_manual and self.state.get_data()['charging']:
            self.scale_charge_power.config(state="normal")
        else:
            self.scale_charge_power.config(state="disabled")
            
        for s in self.env_sliders.values():
            s.config(state=state_str)
        for s in self.tpms_sliders.values():
            s.config(state=state_str)

    def on_slider_change(self, name, val):
        """
        驾驶核心参数滑块被拖动时的回调
        """
        lbl, fmt = self.slider_labels[name]
        f_val = float(val)
        lbl.config(text=fmt % f_val)
        
        if self.is_syncing_ui:
            return
            
        if not self.state.is_script_mode():
            if name == 'battery_level':
                parsed_val = int(f_val)
            else:
                parsed_val = f_val
            self.state.update_data(**{name: parsed_val})

    def on_env_slider_change(self, name, val):
        """
        环境温度滑块被拖动时的回调
        """
        lbl, fmt = self.env_labels[name]
        f_val = float(val)
        lbl.config(text=fmt % f_val)
        
        if self.is_syncing_ui:
            return
            
        if not self.state.is_script_mode():
            self.state.update_data(**{name: f_val})

    def on_tpms_change(self, key, val):
        """
        胎压滑块被拖动时的回调
        """
        lbl = self.tpms_labels[key]
        f_val = float(val)
        lbl.config(text=f"{f_val:.1f} Bar")
        
        if f_val < 2.5 or f_val > 3.2:
            lbl.config(fg=self.COLOR_RED)
        else:
            lbl.config(fg=self.FG_TEXT)
            
        if self.is_syncing_ui:
            return
            
        if not self.state.is_script_mode():
            self.state.update_data(**{key: f_val})

    def set_gear_state(self, target_gear):
        """
        设置挡位。target_gear 为实际传输的单字节字符值：
          'P'/'R'/'N'/'D' 对应标准档位；
          '?' 对应 SNA (Signal Not Available) = 特斯拉 AP 辅助驾驶激活状态。
        """
        if self.state.is_script_mode():
            return
        
        # 档位存储为单字节字符，SNA 使用 '?' 以匹配 ESP32 gear_from_shift_state() 的映射
        self.state.update_data(gear=target_gear)
        self.refresh_gear_ui(target_gear)
        
        display_name = 'SNA (AP)' if target_gear == '?' else target_gear
        self.log_queue.put(f"[Manual] 切换档位至: {display_name}")

    def refresh_gear_ui(self, active_gear):
        """
        更新挡位按钮的高亮指示：
          - SNA ('?') 激活时用蓝色高亮（区分于普通挡位的红色），代表 AP 辅助驾驶状态。
          - 其余档位激活时用红色高亮。
        """
        for char_val, btn in self.gear_buttons.items():
            if char_val == active_gear:
                # SNA 用蓝色区分，其余档位用特斯拉红
                active_color = self.COLOR_BLUE if char_val == '?' else self.COLOR_RED
                btn.config(bg=active_color, fg=self.FG_TEXT)
            else:
                btn.config(bg=self.BG_BTN, fg=self.FG_MUTED)

    def toggle_lock_state(self):
        """
        锁定/解锁车身
        """
        if self.state.is_script_mode():
            return
            
        current = self.state.get_data()['locked']
        new_val = not current
        self.state.update_data(locked=new_val)
        self.refresh_lock_ui(new_val)
        self.log_queue.put(f"[Manual] 车辆中控锁: {'已上锁' if new_val else '已解锁'}")

    def refresh_lock_ui(self, is_locked):
        """
        刷新车锁状态按钮
        """
        if is_locked:
            self.btn_lock.config(text="中控锁定 [锁]", bg=self.COLOR_RED)
        else:
            self.btn_lock.config(text="中控锁定 [开]", bg=self.BG_BTN)

    def toggle_charge_plug(self):
        """
        插拔充电枪控制
        """
        if self.state.is_script_mode():
            return
            
        current = self.state.get_data()['charging']
        new_val = not current
        
        update_dict = {'charging': new_val}
        if not new_val:
            update_dict['charge_power_kw'] = 0.0
            
        self.state.update_data(**update_dict)
        self.refresh_charge_ui(new_val, self.state.get_data()['charge_power_kw'])
        self.log_queue.put(f"[Manual] 充电枪状态: {'连接充电中' if new_val else '已拔出'}")

    def refresh_charge_ui(self, is_charging, power):
        """
        刷新充电状态按钮与滑块启用性
        """
        if is_charging:
            self.btn_charge_plug.config(text="充电枪插拔 [插]", bg=self.COLOR_GREEN)
            if not self.state.is_script_mode():
                self.scale_charge_power.config(state="normal")
        else:
            self.btn_charge_plug.config(text="充电枪插拔 [拔]", bg=self.BG_BTN)
            self.scale_charge_power.config(state="disabled")
            
        self.is_syncing_ui = True
        try:
            self.scale_charge_power.set(power)
        finally:
            self.is_syncing_ui = False
            
        self.lbl_charge_val.config(text=f"{int(power)} kW")

    def on_charge_power_change(self, val):
        """
        拖动充电功率滑块的回调
        """
        power = float(val)
        self.lbl_charge_val.config(text=f"{int(power)} kW")
        
        if self.is_syncing_ui:
            return
            
        if not self.state.is_script_mode():
            self.state.update_data(charge_power_kw=power)

    def toggle_door_state(self, key):
        """
        开启/关闭特定的车门
        """
        if self.state.is_script_mode():
            return
            
        current = self.state.get_data()[key]
        new_val = not current
        self.state.update_data(**{key: new_val})
        self.refresh_door_ui(key, new_val)
        
        door_names = {
            "door_open_fl": "左前门", "door_open_fr": "右前门",
            "door_open_rl": "左后门", "door_open_rr": "右后门",
            "door_open_trunk_front": "前备箱", "door_open_trunk_rear": "后备箱"
        }
        self.log_queue.put(f"[Manual] {door_names.get(key, key)}: {'开启' if new_val else '关闭'}")

    def refresh_door_ui(self, key, is_open):
        """
        刷新对应门控按钮的文本与底色
        """
        btn = self.door_buttons[key]
        clean_text = btn['text'].split(" [")[0]
        if is_open:
            btn.config(text=f"{clean_text} [开]", bg=self.COLOR_RED)
        else:
            btn.config(text=f"{clean_text} [关]", bg=self.BG_BTN)

    # ─── 后台通信同步逻辑 ────────────────────────────────────────
    def periodic_gui_update_tick(self):
        """
        GUI 每 50ms (20Hz) 进行一次生命周期更新，由主协程统一调度推动
        """
        if self.state.is_script_mode():
            self.sync_ui_from_data(self.state.get_data())
            
        self.update_connection_status()
        self.update_emulation_status_ui()
        self.poll_log_queue()

    def update_emulation_status_ui(self):
        q_depth = self.state.get_queue_depth()
        avg_lat = self.state.get_avg_latency()
        fault_until = self.state.get_fault_drop_until()
        now_ts = time.time()
        
        if now_ts < fault_until:
            rem = int(fault_until - now_ts) + 1
            self.lbl_emulation_monitor.config(
                text=f"⚠️ 信号闪断中 ({rem}s) | 队列: {q_depth}/32",
                fg=self.COLOR_RED
            )
        else:
            mode_str = "REAL" if self.state.get_comm_mode() == "REALISTIC" else "IDEAL"
            if q_depth >= 25:
                q_color = self.COLOR_RED
            elif q_depth > 0:
                q_color = "#F59E0B"
            else:
                q_color = self.COLOR_GREEN
            
            self.lbl_emulation_monitor.config(
                text=f"[{mode_str}] 队列: {q_depth}/32 | 延时: ~{int(avg_lat)}ms",
                fg=q_color
            )

        # 配钥请求待授权时高亮横幅，并自动弹出授权窗口
        pending = self.state.get_pair_pending_desc()
        self._update_pair_banner(pending)
        self._sync_pair_dialog(pending)

    def _update_pair_banner(self, pending):
        """配钥横幅：有待授权请求时红色高亮 + 按钮可用，否则灰显。"""
        lbl = getattr(self, "lbl_pair_banner", None)
        btn_ok = getattr(self, "btn_pair_approve", None)
        btn_no = getattr(self, "btn_pair_reject", None)
        if lbl is None:
            return
        if pending:
            lbl.config(text=f"🔑 车辆等待刷卡授权：{pending}", fg=self.COLOR_RED)
            if btn_no is not None:
                btn_no.config(state="normal", bg=self.BG_BTN)
            if btn_ok is not None:
                if self._pair_approve_clicked:
                    btn_ok.config(state="disabled", text="已刷卡，等待车辆确认…")
                else:
                    btn_ok.config(
                        state="normal",
                        text="🔑 轻点钥匙卡（批准）",
                        bg=self.COLOR_GREEN,
                    )
            return
        self._pair_approve_clicked = False
        lbl.config(text="🔑 配钥授权：当前没有待授权的 add-key-request", fg=self.FG_MUTED)
        if btn_ok is not None:
            btn_ok.config(
                state="disabled", text="🔑 轻点钥匙卡（批准）", bg=self.BG_BTN
            )
        if btn_no is not None:
            btn_no.config(state="disabled", bg=self.BG_BTN)

    def sync_ui_from_data(self, data):
        """
        将字典数据全量更新至界面各滑块、按钮
        """
        self.is_syncing_ui = True
        try:
            for name, scale in self.sliders.items():
                scale.set(data[name])
                lbl, fmt = self.slider_labels[name]
                lbl.config(text=fmt % data[name])
                
            for name, scale in self.env_sliders.items():
                scale.set(data[name])
                lbl, fmt = self.env_labels[name]
                lbl.config(text=fmt % data[name])
                
            for name, scale in self.tpms_sliders.items():
                scale.set(data[name])
                lbl = self.tpms_labels[name]
                lbl.config(text=f"{data[name]:.1f} Bar")
                if data[name] < 2.5 or data[name] > 3.2:
                    lbl.config(fg=self.COLOR_RED)
                else:
                    lbl.config(fg=self.FG_TEXT)
                    
            self.refresh_gear_ui(data['gear'])
            self.refresh_lock_ui(data['locked'])
            self.refresh_charge_ui(data['charging'], data['charge_power_kw'])
            
            for key in self.door_buttons.keys():
                self.refresh_door_ui(key, data[key])
                
        finally:
            self.is_syncing_ui = False

    def update_connection_status(self):
        """
        刷新连接状态指示灯和标签
        """
        if not self.state.is_ble_enabled():
            self.status_canvas.itemconfigure(self.status_led, fill=self.COLOR_RED)
            self.lbl_status.config(text="BLE 广播已关闭", fg=self.COLOR_RED)
            return

        is_conn = self.state.is_connected()
        if is_conn:
            self.status_canvas.itemconfigure(self.status_led, fill=self.COLOR_GREEN)
            self.lbl_status.config(text="已连接 (ESP32 配对成功)", fg=self.COLOR_GREEN)
        else:
            self.status_canvas.itemconfigure(self.status_led, fill="#F59E0B")
            self.lbl_status.config(text="等待连接 (广播中...)", fg="#F59E0B")

    def poll_log_queue(self):
        """
        从线程安全队列中消费日志并渲染到控制台
        """
        lines_added = False
        self.txt_log.config(state="normal")
        while not self.log_queue.empty():
            try:
                log_line = self.log_queue.get_nowait()
                timestamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]
                self.txt_log.insert(tk.END, f"[{timestamp}] {log_line}\n")
                lines_added = True
            except queue.Empty:
                break
                
        if lines_added:
            total_lines = int(self.txt_log.index('end-1c').split('.')[0])
            if total_lines > 200:
                self.txt_log.delete("1.0", f"{total_lines - 200}.0")
            self.txt_log.see(tk.END)
            
        self.txt_log.config(state="disabled")

    # ─── 悬停样式辅助 ───────────────────────────────────────────
    def on_btn_hover_enter(self, btn):
        if btn['state'] == 'disabled':
            return
        if btn['bg'] not in [self.COLOR_RED, self.COLOR_GREEN, self.COLOR_BLUE]:
            btn.config(bg=self.BG_BTN_HOVER)

    def on_btn_hover_leave(self, btn, type_group, key):
        if btn['state'] == 'disabled':
            return
        data = self.state.get_data()
        
        if type_group == 'gear':
            if key == data['gear']:
                # SNA ('?') 激活时恢复蓝色，其余档位恢复红色
                restore_color = self.COLOR_BLUE if key == '?' else self.COLOR_RED
                btn.config(bg=restore_color)
            else:
                btn.config(bg=self.BG_BTN)
        elif type_group == 'door':
            if data[key]:
                btn.config(bg=self.COLOR_RED)
            else:
                btn.config(bg=self.BG_BTN)

    def get_current_mode_color(self):
        return self.COLOR_BLUE if self.state.is_script_mode() else self.COLOR_RED

    def adjust_color_brightness(self, hex_color, factor):
        hex_color = hex_color.lstrip('#')
        r = int(hex_color[0:2], 16)
        g = int(hex_color[2:4], 16)
        b = int(hex_color[4:6], 16)
        
        r = min(255, int(r * factor))
        g = min(255, int(g * factor))
        b = min(255, int(b * factor))
        return f"#{r:02x}{g:02x}{b:02x}"


def parse_client_public(spec: str) -> bytes:
    """解析 --client-pub：65 字节非压缩公钥的 130 位 hex（允许 0x 前缀）。"""
    text = spec.strip().lower()
    if text.startswith("0x"):
        text = text[2:]
    if len(text) != 130:
        raise ValueError(f"公钥应为 130 位 hex（65 字节），实际 {len(text)} 位")
    try:
        raw = bytes.fromhex(text)
    except ValueError as exc:
        raise ValueError(f"公钥含非法字符: {exc}") from exc
    if raw[0] != 0x04:
        raise ValueError("公钥必须是非压缩编码（0x04 前缀）")
    return raw


# ─── 主单线程协程异步服务逻辑 ──────────────────────────────────────
async def async_main(args, state: TelemetryState, stop_event: threading.Event, log_queue: queue.Queue):
    """
    统一驱动 GUI 及 BLE 服务的单事件循环核心主协程
    """
    vin = state.get_vin()
    try:
        sha1 = hashlib.sha1(vin.encode('utf-8')).hexdigest()
        ble_name = "S" + sha1[:16] + "C"
    except Exception:
        ble_name = "Tesla-BLE-Simulation"
        
    log_queue.put(f"[+] 特斯拉 BLE 广播名: {ble_name} (基于 VIN: {vin})")
    log_queue.put("[+] 正在配置 BLE 外设服务器...")
    
    last_push_time = 0.0
    current_target_interval = 0.8

    # ── 特斯拉加密协议 real path 初始化 ───────────────────────
    proto_responses = queue.Queue()
    crypto_server = None

    # --client-pub：模拟真车的配对白名单（不传则不校验，兼容旧行为）
    allowed_client_pub = None
    if getattr(args, "client_pub", None):
        try:
            allowed_client_pub = parse_client_public(args.client_pub)
        except ValueError as exc:
            log_queue.put(f"[-] --client-pub 参数非法: {exc}")
            return

    if TeslaCryptoServer is not None:
        crypto_server = TeslaCryptoServer(
            state.get_vin(),
            telemetry=state,
            logger=lambda m: log_queue.put(m),
            allowed_client_public=allowed_client_pub,
            pair_auto=getattr(args, "pair_auto", False),
            pair_timeout_s=getattr(args, "pair_timeout", 300.0),
        )
        crypto_server.response_encryption_enabled = state.is_response_encryption()
        log_queue.put("[+] 特斯拉加密协议栈已就绪 (ECDH P-256 / AES-128-GCM / HMAC-SHA256)")
        if allowed_client_pub is not None:
            log_queue.put(
                f"[+] 已启用配对白名单：仅接受公钥 {allowed_client_pub.hex()[:16]}…"
            )
            if getattr(args, "pair_auto", False):
                log_queue.put("[+] 配钥模式：AUTO（收到 add-key-request 立即授权，模拟刷卡）")
            else:
                log_queue.put(
                    "[+] 配钥模式：NFC（未登记公钥会收到 KEY_NOT_ON_WHITELIST，"
                    "需在界面点击「🔑 轻点钥匙卡」授权）"
                )
    else:
        log_queue.put(f"[-] 加密协议栈不可用（缺少依赖 cryptography）: {TESLA_CRYPTO_ERROR}")

    # 协议响应通知分片大小：兼容 Windows/WinRT 与 Bumble 的最小 MTU
    PROTO_NOTIFY_CHUNK = 20

    def is_complete_frame(data: bytes) -> bool:
        if len(data) < 2:
            return False
        declared = int.from_bytes(data[:2], "big")
        return 0 < declared <= 4096 and declared + 2 == len(data)

    # BLE 客户端特征值写入事件回调
    def on_write(characteristic, value):
        nonlocal last_push_time
        now_ts = time.time()
        fault_until = state.get_fault_drop_until()
        if now_ts < fault_until:
            log_queue.put(f"[BLE-Write] ⚠️ [故障注入中] 丢弃 ESP32 写入请求: {value.hex()[:16]}...")
            return

        data = bytes(value)
        log_queue.put(f"[BLE-Write] 📥 接收 ESP32 指令包 ({len(data)}B): {data.hex()[:24]}...")

        # Fast path：加密关闭，或尚未握手且恰好 53 字节 → 保持原有旁路行为
        if crypto_server is None or not state.is_crypto_enabled():
            log_queue.put("[BLE-Write] 53B 旁路模式：触发即时遥测更新")
            last_push_time = 0.0
            return
        if not crypto_server.sessions and len(data) == 53 and not is_complete_frame(data):
            log_queue.put("[BLE-Write] 53B 旁路模式：触发即时遥测更新")
            last_push_time = 0.0
            return

        # Real path：交给加密协议栈（内部按长度前缀重组分片）
        crypto_server.response_encryption_enabled = state.is_response_encryption()
        try:
            replies = crypto_server.feed(data)
        except Exception as exc:
            log_queue.put(f"[-] 加密协议处理失败: {exc}")
            return
        for reply in replies:
            proto_responses.put(reply)

    async def start_ble_server(name):
        use_bumble = getattr(args, 'bumble', False)

        if use_bumble or HAS_BUMBLE:
            log_queue.put("[+] 使用 Bumble (USB HCI 硬件模式) 启动 BLE 广播...")
            server_obj = BumbleTeslaServer(vin=state.get_vin())
            server_obj.write_callback = on_write
            await server_obj.start()
            return server_obj

        if BlessServer is not None:
            import socket
            pc_name = socket.gethostname()
            if pc_name.lower() == name.lower():
                log_queue.put(f"[+] 电脑名已匹配: {pc_name}，直接以普通模式启动 BLE 广播。")
                server_obj = BlessServer(name=name, name_overwrite=False)
            else:
                log_queue.put(f"[+] 将 Windows 蓝牙设备名覆写为: {name} (需要管理员权限)")
                server_obj = BlessServer(name=name, name_overwrite=True)
            await server_obj.add_new_service(SVC_UUID)
            
            # Write Characteristic (可写特征值)
            await server_obj.add_new_characteristic(
                service_uuid=SVC_UUID,
                char_uuid=WR_UUID,
                properties=GATTCharacteristicProperties.write | GATTCharacteristicProperties.write_without_response,
                value=bytearray(),
                permissions=GATTAttributePermissions.writeable
            )
            
            # Notify Characteristic (主通道特征值)
            await server_obj.add_new_characteristic(
                service_uuid=SVC_UUID,
                char_uuid=RD_UUID,
                properties=GATTCharacteristicProperties.notify,
                value=bytearray(),
                permissions=GATTAttributePermissions.readable
            )
            
            server_obj.write_request_func = on_write
            await server_obj.start()
            return server_obj

        details = []
        if BLESS_IMPORT_ERROR is not None:
            details.append(f"bless 导入失败: {BLESS_IMPORT_ERROR}")
        if BUMBLE_IMPORT_ERROR is not None:
            details.append(f"bumble 导入失败: {BUMBLE_IMPORT_ERROR}")
        detail_text = "；".join(details) if details else "未检测到任何 BLE 后端"
        raise RuntimeError(f"无可用 BLE 后端服务（请安装 bless 或 bumble 依赖）。{detail_text}")

    # 启动蓝牙广播
    try:
        server = await start_ble_server(ble_name)
        log_queue.put("[+] 广播发布成功！等待 ESP32 蓝牙寻址连接...")
    except Exception as e:
        log_queue.put(f"[-] 启动 BLE 广播失败: {e}")
        server = None

    # 初始化 Tkinter 视窗（仅当环境允许且用户没有传递 --nogui 时）
    use_gui = not args.nogui and (tk is not None)
    gui = None
    root = None
    if use_gui:
        root = tk.Tk()
        gui = TeslaSimulatorGUI(root, state, stop_event, log_queue)
        gui.update_controls_interactive_state()
        
        def on_close():
            stop_event.set()
            try:
                root.destroy()
            except Exception:
                pass
        root.protocol("WM_DELETE_WINDOW", on_close)

    frame_log_counter = 0
    last_ble_start_attempt = 0.0
    last_push_time = 0.0
    last_pair_pending_desc = None
    current_target_interval = 0.8
    try:
        while not stop_event.is_set():
            # 1. 推进 Tkinter 图形框架事件循环一次
            if root is not None:
                try:
                    root.update()
                except tk.TclError:
                    stop_event.set()
                    break

            # 2. 消费并打印命令行模式下的日志队列
            if not use_gui:
                while not log_queue.empty():
                    try:
                        print(log_queue.get_nowait())
                    except queue.Empty:
                        break

            # 3. 处理蓝牙广播开关与动态重启（VIN 更改）
            ble_enabled = state.is_ble_enabled()
            if not ble_enabled and server is not None:
                log_queue.put("[System] 用户关闭了 BLE 蓝牙广播...")
                try:
                    await server.stop()
                except Exception as e:
                    log_queue.put(f"[-] 停止 BLE 服务时发生异常: {e}")
                server = None
                state.set_connected(False)
            elif ble_enabled and server is None and not state.check_restart():
                now_t = time.time()
                if now_t - last_ble_start_attempt >= 2.0:
                    last_ble_start_attempt = now_t
                    log_queue.put("[System] 正在启动 BLE 蓝牙广播服务...")
                    try:
                        server = await start_ble_server(ble_name)
                        log_queue.put("[+] 广播开启成功！等待 ESP32 蓝牙寻址连接...")
                    except Exception as e:
                        log_queue.put(f"[-] 启动 BLE 广播失败: {e}")
                        server = None

            if state.check_restart():
                if not ble_enabled:
                    log_queue.put("[System] 提示: 蓝牙广播当前为关闭状态。已保存新 VIN，开启广播后生效。")
                    if gui is not None:
                        gui.btn_restart_ble.config(state="normal", text="应用并重启广播")
                else:
                    log_queue.put("[System] 正在关闭现有蓝牙服务...")
                    if server is not None:
                        try:
                            await server.stop()
                        except Exception:
                            pass
                    
                    new_vin = state.get_vin()
                    try:
                        sha1 = hashlib.sha1(new_vin.encode('utf-8')).hexdigest()
                        new_ble_name = "S" + sha1[:16] + "C"
                    except Exception:
                        new_ble_name = "Tesla-BLE-Simulation"
                    
                    log_queue.put(f"[System] 重新配置蓝牙广播名: {new_ble_name}")
                    if crypto_server is not None:
                        crypto_server.set_vin(new_vin)
                        crypto_server.reset()
                        log_queue.put("[CRYPTO] VIN 变更，已清空旧加密会话")
                    try:
                        server = await start_ble_server(new_ble_name)
                        log_queue.put("[+] 广播重启发布成功！等待连接...")
                    except Exception as e:
                        log_queue.put(f"[-] 蓝牙广播重启失败: {e}")
                        server = None
                    
                    if gui is not None:
                        gui.btn_restart_ble.config(state="normal", text="应用并重启广播")

            # 4. 加密协议：处理「使会话失效」、配钥授权并发送待回包
            if crypto_server is not None:
                if state.consume_crypto_reset():
                    crypto_server.reset()
                    log_queue.put("[CRYPTO] 已使会话失效（模拟车机重启），等待 ESP32 重新握手")

                # 配钥：界面「轻点钥匙卡」→ 批准；超时由状态机自行回终态
                approve, reject = state.consume_pair_requests()
                if approve:
                    if crypto_server.pairing_approve():
                        log_queue.put("[PAIR] 🔑 NFC 刷卡成功，钥匙已写入车端白名单")
                    else:
                        log_queue.put("[PAIR] 当前没有待授权的配钥请求，忽略刷卡")
                if reject and crypto_server.pairing_reject():
                    log_queue.put("[PAIR] 已拒绝本次配钥请求（UI DENIED）")
                pending = crypto_server.pairing_pending()
                state.set_pair_pending_desc(pending)
                if pending and pending != last_pair_pending_desc:
                    log_queue.put(
                        f"[PAIR] ⏳ 车辆等待刷卡：{pending}（点击界面「🔑 轻点钥匙卡」）"
                    )
                last_pair_pending_desc = pending

                # 刷卡终态 / 超时等异步回包
                for event in crypto_server.poll_events():
                    proto_responses.put(event)

                while not proto_responses.empty():
                    try:
                        reply = proto_responses.get_nowait()
                    except queue.Empty:
                        break
                    try:
                        if server is not None and hasattr(server, 'notify'):
                            for i in range(0, len(reply), PROTO_NOTIFY_CHUNK):
                                await server.notify(reply[i:i + PROTO_NOTIFY_CHUNK])
                        elif server is not None:
                            char = server.get_characteristic(RD_UUID)
                            for i in range(0, len(reply), PROTO_NOTIFY_CHUNK):
                                char.value = reply[i:i + PROTO_NOTIFY_CHUNK]
                                server.update_value(SVC_UUID, RD_UUID)
                    except Exception as e:
                        log_queue.put(f"[-] 发送加密响应失败: {e}")

            # 5. 遥测数据推送机制 (支持真车节奏分批推送 vs 理想 20Hz 连续推送)
            if server is not None:
                is_conn = await server.is_connected()
                state.set_connected(is_conn)
                
                if is_conn:
                    now_t = time.time()
                    if crypto_server is not None and state.is_crypto_enabled() and crypto_server.sessions:
                        # 加密 real path：遥测由 ESP32 的 GetVehicleData 请求驱动，不主动推 53B
                        pass
                    elif state.get_comm_mode() == "REALISTIC":
                        # 真实车机节奏模式 (方案 A)：按 800ms 基础间隔 (带延迟/抖动/丢包) 分批推送 53 字节 SimPayload
                        if now_t - last_push_time >= current_target_interval:
                            last_push_time = now_t

                            # 基础推送间隔 800ms
                            base_interval = 0.8
                            if random.random() < 0.05:
                                base_interval *= 2.0
                                log_queue.put("[BLE-Server] ⚡ 触发 5% BLE 链路长尾抖动 (推送间隔翻倍)")

                            current_target_interval = base_interval * state.get_delay_multiplier()

                            # 检查故障注入/闪断
                            if now_t < state.get_fault_drop_until():
                                log_queue.put("[BLE-Server] ⚠️ [故障注入中] 暂停遥测数据推送")
                            # 模拟丢包
                            elif state.get_loss_rate() > 0 and random.random() < state.get_loss_rate():
                                log_queue.put(f"[BLE-Server] 📉 模拟 BLE 无线链路丢包, 本批次跳过推送")
                            else:
                                if state.is_script_mode():
                                    frame = state.increment_frame()
                                    mock_data = generate_mock_frame(frame)
                                    state.update_data(**mock_data)
                                else:
                                    mock_data = state.get_data()
                                    mock_data['script_phase_desc'] = "真实节奏模式"

                                payload = pack_telemetry(mock_data)
                                try:
                                    if hasattr(server, 'notify'):
                                        await server.notify(payload)
                                    else:
                                        server.get_characteristic(RD_UUID).value = payload
                                        server.update_value(SVC_UUID, RD_UUID)
                                    
                                    state.add_latency_record(current_target_interval * 1000)
                                    log_queue.put(f"[BLE-Notify] 📤 真实节奏推送完成: Speed={mock_data['speed_kmh']:.1f} KM/H, Gear={mock_data['gear']} (下次间隔: {current_target_interval*1000:.0f}ms)")
                                except Exception as e:
                                    log_queue.put(f"[-] 推送遥测数据出错: {e}")
                    else:
                        # 理想 20Hz 主动推送模式
                        if state.is_script_mode():
                            frame = state.increment_frame()
                            mock_data = generate_mock_frame(frame)
                            state.update_data(**mock_data)
                        else:
                            mock_data = state.get_data()
                            mock_data['script_phase_desc'] = "手动干预模式"
                        
                        payload = pack_telemetry(mock_data)
                        try:
                            if hasattr(server, 'notify'):
                                await server.notify(payload)
                            else:
                                server.get_characteristic(RD_UUID).value = payload
                                server.update_value(SVC_UUID, RD_UUID)
                        except Exception as e:
                            log_queue.put(f"[-] 推送遥测数据出错: {e}")
                        
                        if frame_log_counter % 40 == 0:
                            log_queue.put(f"[BLE-Notify] 理想推送: Speed={mock_data['speed_kmh']:.1f} KM/H, Gear={mock_data['gear']}")
                        frame_log_counter += 1
                else:
                    if frame_log_counter > 0:
                        log_queue.put("[-] ESP32 断开蓝牙连接，重新等待连接中...")
                        frame_log_counter = 0

            # 5. 更新 GUI 生命周期的定时状态
            if gui is not None and not stop_event.is_set():
                gui.periodic_gui_update_tick()

            await asyncio.sleep(0.05)  # 20Hz 主循环周期 (50ms)
            
    except Exception as e:
        log_queue.put(f"[-] 主协程循环发生异常: {e}")
    finally:
        if server is not None:
            log_queue.put("[+] 正在停止 BLE 广播服务器...")
            try:
                await server.stop()
                log_queue.put("[+] BLE 广播已安全关闭。")
            except Exception as e:
                log_queue.put(f"[-] 停止 BLE 服务时发生异常: {e}")


# ─── 程序主入口 ──────────────────────────────────────────────
if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Tesla BLE HMI Windows Simulator Control Server")
    parser.add_argument("--nogui", action="store_true", help="强制在纯命令行模式下运行（不启动 Tkinter 界面）")
    parser.add_argument("--vin", type=str, default="LRWYGCFJ8RC571213", help="设定模拟车型的 17 位 VIN 识别号（默认: LRWYGCFJ8RC571213）")
    parser.add_argument("--bumble", action="store_true", help="使用 Bumble (CSR8510 USB HCI) 硬件物理广播模式")
    parser.add_argument(
        "--client-pub",
        type=str,
        default="",
        help="启用车端配对白名单：只接受该客户端公钥（65 字节非压缩编码的 130 位 hex，可带 0x 前缀）",
    )
    parser.add_argument(
        "--pair-auto",
        action="store_true",
        help="配钥自动授权：收到 add-key-request 立即写入白名单（不模拟 NFC 刷卡）",
    )
    parser.add_argument(
        "--pair-timeout",
        type=float,
        default=300.0,
        help="等待 NFC 刷卡的超时秒数（超时回 TIMED_OUT_WAITING_FOR_TAP），默认 300",
    )
    args = parser.parse_args()
    
    print("==========================================================")
    print("      Tesla BLE HMI Windows Simulator Server v2.0 Stable  ")
    print("==========================================================")
    
    # 强制在 Windows 平台下将 asyncio loop 初始化为 SelectorLoop (防止 Proactor 冲突与死锁)
    if sys.platform == 'win32':
        try:
            import warnings
            with warnings.catch_warnings():
                warnings.simplefilter('ignore', DeprecationWarning)
                asyncio.set_event_loop_policy(asyncio.WindowsSelectorEventLoopPolicy())
        except Exception:
            pass
        
    state = TelemetryState()
    state.set_vin(args.vin)
    
    stop_event = threading.Event()
    log_queue = queue.Queue()
    
    try:
        asyncio.run(async_main(args, state, stop_event, log_queue))
    except KeyboardInterrupt:
        print("\n[+] 接收到 Ctrl+C 中断信号，正在安全退出...")
    except Exception as e:
        print(f"[-] 进程启动失败，异常原因: {e}")
    print("[+] 仿真服务器已完全退出。")
