"""
Tesla BLE HMI Simulator - Bumble USB HCI Server Backend
Uses pure Python Bumble BLE stack to talk directly to CSR8510 (WinUSB) adapter via HCI.
Bypasses all Windows OS Bluetooth restrictions and broadcasts genuine BLE over-the-air Local Name.
"""

import sys
import time
import struct
import asyncio
import hashlib
from bumble.device import Device
from bumble.gatt import Service, Characteristic
from bumble.core import UUID
from bumble.transport import open_transport
from bumble.host import Host

SVC_UUID = "00000211-b2d1-43f0-9b88-960cebf8b91e"
WR_UUID  = "00000212-b2d1-43f0-9b88-960cebf8b91e"
RD_UUID  = "00000213-b2d1-43f0-9b88-960cebf8b91e"

PAYLOAD_FORMAT = "<ffifc6BBBfff4f"

class BumbleTeslaServer:
    def __init__(self, vin="LRWYGCFS2PC792568", usb_spec="usb:0a12:0001"):
        self.vin = vin
        self.usb_spec = usb_spec
        sha1 = hashlib.sha1(vin.encode('utf-8')).hexdigest()
        self.ble_name = "S" + sha1[:16] + "C"
        
        self.device = None
        self.host = None
        self.notify_char = None
        self.write_char = None
        self.connected_peers = set()
        self.write_callback = None

    async def start(self):
        print(f"[Bumble] 正在初始化 CSR8510 USB 蓝牙适配器 ({self.usb_spec})...")
        transport = await open_transport(self.usb_spec)
        self.host = Host(transport.source, transport.sink)
        
        # 建立 Bumble BLE 设备
        self.device = Device(
            address='F4:4E:FC:14:3C:A7',
            name=self.ble_name,
            host=self.host
        )
        
        # 构建空中真实的 BLE 广播与扫描回复载荷
        # 1. 广播数据: Flags (3字节) + Service UUID 128位 (18字节) = 21 字节 (小于31字节)
        self.device.advertising_data = bytes(
            [2, 0x01, 0x06] +
            [17, 0x06] + list(bytes.fromhex('1eb9f8eb0c96889bf043d1b211020000'))
        )
        # 2. 扫描回复数据: Complete Local Name (20字节) -> 包含完整的 Sa7785a96101d0c3fC 名字！
        self.device.scan_response_data = bytes(
            [len(self.ble_name) + 1, 0x09] + list(bytes(self.ble_name, 'utf-8'))
        )
        
        # 配置 GATT 特征值
        self.write_char = Characteristic(
            UUID(WR_UUID),
            Characteristic.Properties.WRITE | Characteristic.Properties.WRITE_WITHOUT_RESPONSE,
            Characteristic.Permissions.WRITEABLE,
            bytes()
        )
        
        if self.write_callback:
            @self.write_char.on('write')
            def _on_write(connection, value):
                self.write_callback(self.write_char, value)

        self.notify_char = Characteristic(
            UUID(RD_UUID),
            Characteristic.Properties.READ | Characteristic.Properties.NOTIFY,
            Characteristic.Permissions.READABLE,
            bytes()
        )
        
        service = Service(UUID(SVC_UUID), [self.write_char, self.notify_char])
        self.device.add_service(service)
        
        # 监听连接与断开事件
        @self.device.on('connection')
        def on_connection(connection):
            print(f"[Bumble] ESP32 已建立物理层 BLE 连接: {connection}")
            self.connected_peers.add(connection)
            
        @self.device.on('disconnection')
        def on_disconnection(connection, reason):
            print(f"[Bumble] ESP32 断开 BLE 连接, 原因代码: {reason}")
            self.connected_peers.discard(connection)

        await self.device.power_on()
        await self.device.start_advertising()
        print(f"[Bumble] [+] 特斯拉 BLE 空中广播启动成功！广播名: {self.ble_name}")
        print(f"[Bumble] [+] 物理广播数据已载入。ESP32 主机随时可扫出并发起连接。")

    async def is_connected(self):
        return len(self.connected_peers) > 0

    async def notify(self, payload_bytes):
        if self.notify_char and self.connected_peers:
            for conn in list(self.connected_peers):
                await self.device.notify_subscriber(conn, self.notify_char, payload_bytes)

    async def stop(self):
        if self.device:
            await self.device.stop_advertising()
            print("[Bumble] BLE 广播服务已停止。")

def pack_telemetry(data):
    gear_byte = data['gear'].encode('ascii') if isinstance(data['gear'], str) else data['gear']
    doors_bytes = bytes(data['doors'])
    
    return struct.pack(
        PAYLOAD_FORMAT,
        float(data['speed_kmh']),
        float(data['motor_power_kw']),
        int(data['battery_level']),
        float(data['battery_range_km']),
        gear_byte,
        doors_bytes,
        int(data['locked']),
        int(data['charging']),
        float(data['charge_power_kw']),
        float(data['inside_temp']),
        float(data['outside_temp']),
        float(data['tpms'][0]),
        float(data['tpms'][1]),
        float(data['tpms'][2]),
        float(data['tpms'][3])
    )

if __name__ == '__main__':
    print("=" * 60)
    print("  Tesla BLE HMI Simulator - Bumble USB HCI Backend")
    print("=" * 60)
    
    async def standalone_test():
        server = BumbleTeslaServer()
        await server.start()
        
        mock_data = {
            'speed_kmh': 0.0,
            'motor_power_kw': 0.0,
            'battery_level': 65,
            'battery_range_km': 380.0,
            'gear': 'P',
            'doors': [0, 0, 0, 0, 0, 0],
            'locked': 1,
            'charging': 0,
            'charge_power_kw': 0.0,
            'inside_temp': 22.0,
            'outside_temp': 26.0,
            'tpms': [2.9, 2.9, 2.9, 2.9]
        }
        
        count = 0
        while True:
            await asyncio.sleep(0.05)
            if await server.is_connected():
                payload = pack_telemetry(mock_data)
                await server.notify(payload)
                if count % 40 == 0:
                    print(f"[BLE-Notify] 推送给 ESP32: Speed={mock_data['speed_kmh']} KM/H, Gear={mock_data['gear']}")
                count += 1
                
    try:
        asyncio.run(standalone_test())
    except KeyboardInterrupt:
        print("\n[+] 退出测试。")
