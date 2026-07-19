import sys
import time
import math
import struct
import argparse
import asyncio
import threading
import queue
import hashlib
from datetime import datetime

# BLE 模拟库导入保护
try:
    from bless import (
        BlessServer,
        BlessGATTCharacteristic,
        GATTCharacteristicProperties,
        GATTAttributePermissions
    )
except ImportError:
    BlessServer = None

# Tkinter 导入保护
try:
    import tkinter as tk
    from tkinter import ttk, messagebox
except ImportError:
    tk = None

# ─── 特斯拉自定义 BLE UUID ──────────────────────────────────────
SVC_UUID = "00000211-b2d1-43f0-9b88-960cebf8b91e"
WR_UUID  = "00000212-b2d1-43f0-9b88-960cebf8b91e"
RD_UUID  = "00000213-b2d1-43f0-9b88-960cebf8b91e"

# ─── 数据载荷打包格式 ──────────────────────────────────────────
PAYLOAD_FORMAT = "<ffifc6BBfff4f"

# ─── 线程安全共享遥测状态 ─────────────────────────────────────
class TelemetryState:
    def __init__(self):
        self.lock = threading.Lock()
        self.use_script_mode = True   # 是否使用剧本模式 (默认为 True)
        self.ble_connected = False    # BLE 连接状态
        self.frame_counter = 0        # 剧本帧计数器
        self.vin = "LRWYGCFS2PC792568" # 默认匹配 ESP32 默认固件的 VIN

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


def pack_telemetry(data):
    """
    将字典数据序列化为紧凑的二进制 SimPayload
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
    20Hz 刷新下，全长剧本运行时间约为 51.5 秒。
    """
    loop_frame = frame_counter % 1030
    
    # 默认基础状态
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
        data['tpms_fl'] = 2.4  # 低压报警
        data['tpms_fr'] = 2.8
        data['tpms_rl'] = 3.3  # 高压报警
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
            data['motor_power_kw'] = -12.5  # 轻度回收
        else:
            ratio = (loop_frame - 410) / 50.0
            data['speed_kmh'] = 90.0 - ratio * 80.0
            data['motor_power_kw'] = -12.5 - ratio * 47.5  # 强力回收
        data['tpms_fl'] = 2.4
        data['tpms_fr'] = 3.1
        data['tpms_rl'] = 3.3
        data['tpms_rr'] = 3.3
        data['script_phase_desc'] = "阶段 4: 强力能量回收制动"

    # 阶段 5：巡航模式 FSD (定速 80 ~ 85)（460 ~ 580帧）
    elif 580 > loop_frame >= 460:
        data['locked'] = False
        data['gear'] = '?'  # 巡航档
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


# ─── 后台 BLE 广播任务 ──────────────────────────────────────
async def run_ble_simulation(state: TelemetryState, stop_event: threading.Event, log_queue: queue.Queue):
    """
    低功耗蓝牙 BLE 广播仿真服务
    """
    if not BlessServer:
        log_queue.put("[-] 错误: 未安装 bless 蓝牙外设模拟库，请运行 pip install -r requirements.txt 安装。")
        return

    vin = state.get_vin()
    try:
        sha1 = hashlib.sha1(vin.encode('utf-8')).hexdigest()
        ble_name = "S" + sha1[:16] + "C"
    except Exception as e:
        ble_name = "Tesla-BLE-Simulation"
        log_queue.put(f"[-] 计算 VIN 广播名失败，采用默认名字: {e}")

    log_queue.put(f"[+] 特斯拉 BLE 广播名: {ble_name} (基于 VIN: {vin})")
    log_queue.put("[+] 正在配置 BLE 外设服务器...")
    try:
        server = BlessServer(name=ble_name)
    except Exception as e:
        log_queue.put(f"[-] 初始化 BLE 服务器失败: {e}")
        return

    # 接收客户端写入控制
    def on_write(characteristic, value):
        log_queue.put(f"[BLE-Write] 接收到控制命令: {value.hex()}")

    try:
        server.add_new_service(SVC_UUID)
        
        # Write Characteristic (可写特征值)
        server.add_new_characteristic(
            service_uuid=SVC_UUID,
            char_uuid=WR_UUID,
            properties=GATTCharacteristicProperties.write | GATTCharacteristicProperties.write_without_response,
            value=bytearray(),
            permissions=GATTAttributePermissions.writeable
        )
        
        # Notify Characteristic (主通道特征值)
        server.add_new_characteristic(
            service_uuid=SVC_UUID,
            char_uuid=RD_UUID,
            properties=GATTCharacteristicProperties.notify,
            value=bytearray(),
            permissions=GATTAttributePermissions.readable
        )
        
        server.write_callbacks[WR_UUID] = on_write
    except Exception as e:
        log_queue.put(f"[-] 创建 BLE 服务/特征值异常: {e}")
        return

    log_queue.put(f"[+] 启动广播: Service UUID = {SVC_UUID}")
    try:
        await server.start()
        log_queue.put("[+] 广播发布成功！等待 ESP32 蓝牙寻址连接...")
    except Exception as e:
        log_queue.put(f"[-] 启动 BLE 广播失败: {e}. 请检查您的电脑蓝牙是否已在系统设置中开启。")
        return

    frame_log_counter = 0
    try:
        while not stop_event.is_set():
            is_conn = server.is_connected
            state.set_connected(is_conn)
            
            if is_conn:
                if state.is_script_mode():
                    # 剧本模式下，自增帧数并更新遥测值
                    frame = state.increment_frame()
                    mock_data = generate_mock_frame(frame)
                    state.update_data(**mock_data)
                else:
                    # 手动模式下，直接读取状态机中的当前值
                    mock_data = state.get_data()
                    mock_data['script_phase_desc'] = "手动干预模式"
                
                # 数据打包并发送
                payload = pack_telemetry(mock_data)
                server.get_characteristic(RD_UUID).value = payload
                server.update_value(SVC_UUID, RD_UUID)
                
                # 每 2 秒 (40 帧) 在日志中输出一次信息
                if frame_log_counter % 40 == 0:
                    log_queue.put(f"[BLE-Notify] 推送: Speed={mock_data['speed_kmh']:.1f} KM/H, Gear={mock_data['gear']}, SOC={mock_data['battery_level']}%, Phase={mock_data.get('script_phase_desc', '手动')}")
                frame_log_counter += 1
            else:
                if frame_log_counter > 0:
                    log_queue.put("[-] ESP32 断开蓝牙连接，重新等待连接中...")
                    frame_log_counter = 0
            
            await asyncio.sleep(0.05)  # 20Hz 发送频率 (50ms 周期)
    except Exception as e:
        log_queue.put(f"[-] BLE 主循环异常: {e}")
    finally:
        log_queue.put("[+] 正在停止 BLE 广播服务器...")
        try:
            await server.stop()
            log_queue.put("[+] BLE 广播已安全关闭。")
        except Exception as e:
            log_queue.put(f"[-] 停止 BLE 服务时发生异常: {e}")


def ble_thread_worker(state, stop_event, log_queue):
    """
    负责运行 asyncio 蓝牙事件循环的后台线程工作器
    """
    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    try:
        loop.run_until_complete(run_ble_simulation(state, stop_event, log_queue))
    finally:
        loop.close()


# ─── 特斯拉深色主题 GUI 界面类 ────────────────────────────────
class TeslaSimulatorGUI:
    def __init__(self, root, state: TelemetryState, stop_event: threading.Event, log_queue: queue.Queue):
        self.root = root
        self.state = state
        self.stop_event = stop_event
        self.log_queue = log_queue
        
        # 标志位：指示是否正在将后台数据同步到 UI，用以防止滑块的 Command 回调循环触发
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
        self.root.title("Tesla BLE TFT 仪表盘 - 仿真控制器")
        self.root.geometry("1120x720")
        self.root.minsize(1050, 680)
        self.root.config(bg=self.BG_MAIN)
        
        # 全局网格权重
        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(1, weight=1)
        
        # 初始化界面布局
        self.create_top_panel()
        self.create_main_content()
        
        # 绑定窗口关闭事件
        self.root.protocol("WM_DELETE_WINDOW", self.on_close)
        
        # 启动 GUI 的定时轮询器 (100ms 刷新率)
        self.root.after(100, self.periodic_gui_update)
        
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
        
        # 模式切换大 Toggle 按钮
        self.btn_mode = tk.Button(
            top_frame,
            text="自动剧本模式 (PLAYING)",
            font=("Segoe UI", 10, "bold"),
            bg=self.COLOR_BLUE,
            fg=self.FG_TEXT,
            activebackground=self.COLOR_BLUE,
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
            messagebox.showerror("错误", "特斯拉 VIN 必须为 17 位字符！\n示例: LRWYGCFS2PC792568")
            return
        
        self.state.set_vin(new_vin)
        self.log_queue.put(f"[System] 正在请求重启蓝牙广播，新 VIN: {new_vin}")
        
        # 禁用重启按钮，防止重复点击
        self.btn_restart_ble.config(state="disabled", text="重启中...")
        
        # 停止旧的线程
        self.stop_event.set()
        
        def restart_worker():
            if hasattr(self.root, 'ble_thread') and self.root.ble_thread.is_alive():
                self.root.ble_thread.join(timeout=3.0)
            
            # 重置停止标志
            self.stop_event.clear()
            
            # 启动新线程
            self.root.ble_thread = threading.Thread(
                target=ble_thread_worker, 
                args=(self.state, self.stop_event, self.log_queue), 
                daemon=True
            )
            self.root.ble_thread.start()
            self.log_queue.put("[System] 蓝牙广播已重启完成。")
            
            # 恢复按钮状态
            self.root.after(0, lambda: self.btn_restart_ble.config(state="normal", text="应用并重启广播"))
            
        threading.Thread(target=restart_worker, daemon=True).start()

    def create_main_content(self):
        """
        创建中部核心网格布局
        """
        main_frame = tk.Frame(self.root, bg=self.BG_MAIN)
        main_frame.grid(row=1, column=0, sticky="nsew", padx=10, pady=5)
        
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
        for g in ['P', 'R', 'N', 'D']:
            btn = tk.Button(
                gear_frame,
                text=g,
                font=("Segoe UI", 10, "bold"),
                width=4,
                bg=self.BG_BTN,
                fg=self.FG_TEXT,
                activebackground=self.COLOR_RED,
                activeforeground=self.FG_TEXT,
                bd=0,
                relief="flat",
                command=lambda target_gear=g: self.set_gear_state(target_gear)
            )
            btn.pack(side="left", padx=5)
            btn.bind("<Enter>", lambda e, b=btn: self.on_btn_hover_enter(b))
            btn.bind("<Leave>", lambda e, b=btn: self.on_btn_hover_leave(b, 'gear', btn['text']))
            self.gear_buttons[g] = btn
            
        # 2. 中控锁与充电状态 Toggle 按钮
        toggle_frame = tk.Frame(container, bg=self.BG_CARD)
        toggle_frame.pack(fill="x", pady=5)
        
        self.btn_lock = self.create_toggle_btn(toggle_frame, "中控锁定 [关]", self.toggle_lock_state)
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
        设置挡位
        """
        if self.state.is_script_mode():
            return
        
        self.state.update_data(gear=target_gear)
        self.refresh_gear_ui(target_gear)
        self.log_queue.put(f"[Manual] 切换档位至: {target_gear}")

    def refresh_gear_ui(self, active_gear):
        """
        更新挡位按钮的高亮指示
        """
        for g, btn in self.gear_buttons.items():
            if g == active_gear:
                btn.config(bg=self.COLOR_RED, fg=self.FG_TEXT)
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
    def periodic_gui_update(self):
        """
        GUI 每 100ms 进行一次的生命周期轮询，负责状态更新与日志提取
        """
        if self.state.is_script_mode():
            self.sync_ui_from_data(self.state.get_data())
            
        self.update_connection_status()
        self.poll_log_queue()
        
        if not self.stop_event.is_set():
            self.root.after(100, self.periodic_gui_update)

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
                btn.config(bg=self.COLOR_RED)
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

    def on_close(self):
        self.stop_event.set()
        self.root.destroy()


# ─── 命令行无 GUI 主循环模式 ─────────────────────────────────
def run_cli_mode(state: TelemetryState, stop_event: threading.Event, log_queue: queue.Queue):
    """
    原版无 GUI 兼容模式：直接拉起异步 BLE 服务器，并在主线程中消费队列打印日志
    """
    print("[+] 正在无 GUI 模式下启动 Tesla BLE 遥测仿真器...")
    
    ble_thread = threading.Thread(target=ble_thread_worker, args=(state, stop_event, log_queue), daemon=True)
    ble_thread.start()
    
    try:
        while True:
            while not log_queue.empty():
                try:
                    log_line = log_queue.get_nowait()
                    print(log_line)
                except queue.Empty:
                    break
            time.sleep(0.1)
    except KeyboardInterrupt:
        print("\n[+] 正在退出，正在停止 BLE 服务...")
        stop_event.set()
        ble_thread.join(timeout=2.0)
        print("[+] 退出成功。")


# ─── 程序主入口 ──────────────────────────────────────────────
if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Tesla BLE HMI Windows Simulator Control Server")
    parser.add_argument("--nogui", action="store_true", help="强制在纯命令行模式下运行（不启动 Tkinter 界面）")
    parser.add_argument("--vin", type=str, default="LRWYGCFS2PC792568", help="设定模拟车型的 17 位 VIN 识别号（默认为 LRWYGCFS2PC792568）")
    args = parser.parse_args()
    
    print("==========================================================")
    print("      Tesla BLE HMI Windows Simulator Server v2.1         ")
    print("==========================================================")
    
    state = TelemetryState()
    # 依据参数（或GUI默认）初始化 VIN
    state.set_vin(args.vin)
    
    stop_event = threading.Event()
    log_queue = queue.Queue()
    
    use_gui = not args.nogui and (tk is not None)
    
    if not use_gui:
        if args.nogui:
            log_queue.put("[System] 用户指定了 --nogui，采用命令行运行。")
        else:
            log_queue.put("[System] 环境缺失 tkinter 组件，强制切换至命令行模式。")
        run_cli_mode(state, stop_event, log_queue)
    else:
        # GUI 模式运行
        ble_thread = threading.Thread(target=ble_thread_worker, args=(state, stop_event, log_queue), daemon=True)
        ble_thread.start()
        
        root = tk.Tk()
        # 绑定蓝牙线程引用在 root 上，以便 GUI 内部执行重启服务
        root.ble_thread = ble_thread
        gui = TeslaSimulatorGUI(root, state, stop_event, log_queue)
        
        try:
            root.mainloop()
        except KeyboardInterrupt:
            stop_event.set()
            root.destroy()
        
        ble_thread.join(timeout=2.0)
        print("[+] 仿真服务器已完全退出。")
