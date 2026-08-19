# 普通 USB 蓝牙接收器硬件改造与配置指南 (USB Bluetooth Dongle + Zadig + Bumble)

本指南旨在指导开发者与用户，如何使用一个成本约 10~15 元人民币的普通 USB 蓝牙接收器（USB Bluetooth Dongle），通过 **Zadig 驱动替换** 与 **Python Bumble 原生 HCI 驱动**，将其改造为一个能够被本系统（Tesla BLE Simulator）全权掌控的特斯拉车机 BLE 硬件信号发射源。

---

## 💡 为什么需要使用 USB 蓝牙接收器 + Zadig？

在 Windows 操作系统下，传统的系统自带蓝牙服务（UWP / WinRT BLE API）对 BLE 外设（Peripheral）广播有极其严格的权限限制：
1. **广播名称锁定**：Windows 不允许第三方 Python 脚本随心所欲地覆写广播名称（如特斯拉特定计算的 `S<sha1_prefix>C`）。
2. **广播频率限制**：系统级 API 会强制对 BLE 广播和 Notify 增加缓冲延迟，无法精准模拟真实车机 800ms 分批响应节奏。

通过使用 **Zadig** 将 USB 蓝牙适配器的原生 Windows 驱动替换为 **WinUSB 通用驱动**，Python 脚本能够使用 **Bumble** 库绕过 Windows 蓝牙协议栈，直接通过 USB 物理接口（HCI 端口）发送原生的蓝牙空中数据包，从而获得与真实特斯拉车机 100% 一致的 BLE 空中广播与数据通信能力！

---

## 🛠️ 1. 硬件准备

* **推荐设备**：基于 **CSR8510 A10** 芯片的 USB 蓝牙 4.0 / 5.0 适配器（淘宝/京东搜索“CSR8510 蓝牙适配器”，单价约 10~15 元）。
* **兼容芯片**：大多数基于 CSR8510 (USB VID: `0a12`, PID: `0001`) 的蓝牙适配器均可即插即用。
* **双适配器建议**：如果您的电脑本身带有板载蓝牙（如笔记本电脑的 Intel 蓝牙），建议**保留板载蓝牙用于连接电脑蓝牙鼠标/耳机**，专门插上一个独立的 USB 蓝牙接收器用于运行特斯拉模拟器。

---

## 🚀 2. Zadig 驱动配置步骤 (Step-by-Step)

### 第一步：插入 USB 蓝牙接收器
将 USB 蓝牙接收器插入 Windows 电脑的 USB 接口。

### 第二步：下载并运行 Zadig
1. 访问 Zadig 官方网站下载软件：[https://zadig.akeo.ie/](https://zadig.akeo.ie/) (软件绿色免安装)。
2. 右键选择 **“以管理员身份运行”** `zadig-x.x.exe`。

### 第三步：在 Zadig 中列出所有 USB 设备
1. 点击顶部菜单栏的 **`Options`**。
2. 勾选 **`List All Devices`**（列出所有设备）。

<img width="596" height="260" alt="20260819194904_19_341" src="https://github.com/user-attachments/assets/b086a94a-3c56-4d76-9964-38dcc0712c46" />


### 第四步：选择蓝牙适配器并替换驱动
1. 在下拉选择框中，选中您的 USB 蓝牙接收器，通常显示为 **`CSR8510 A10`** 或 **`Generic Bluetooth Adapter`**。
   > ⚠️ **注意**：请仔细核对 USB ID，通常为 `0A12 0001`。切勿误选鼠标、键盘或系统主卡！
   <img width="631" height="295" alt="20260819195050_20_341" src="https://github.com/user-attachments/assets/cb30408b-a5ff-4512-9567-74535eda0981" />

2. 在 **`Driver`** 对应的右侧目标驱动下拉框中，选择 **`WinUSB (v6.1.7600.16385)`**。

3. 点击大大的 **`Replace Driver`**（或 `Install Driver`）按钮。

4. 等待 10~30 秒，直到弹窗提示 **“The driver was successfully updated.”**，即代表硬件改造成功！

---

## 📡 3. 运行特斯拉模拟器

完成 Zadig 驱动替换后，USB 蓝牙接收器已归 Bumble 原生驱动接管。

在 `Simulation_app` 目录下直接运行：

```powershell
python sim_server.py
```

或者使用一键批处理：
```powershell
.\run.bat
```

系统会自动检测到可用的 Bumble HCI 设备，控制台将输出如下成功日志：

```text
[BLE] Windows BLEAdapter 延迟补丁应用成功。
==========================================================
      Tesla BLE HMI Windows Simulator Server v2.0 Stable
==========================================================
[Bumble] 正在初始化 CSR8510 USB 蓝牙适配器 (usb:0a12:0001)...
[Bumble] [+] 特斯拉 BLE 空中广播启动成功！广播名: S7e6a716c52bbcd2cC
[+] 广播发布成功！等待 ESP32 蓝牙寻址连接...
```

此时，烧录好固件的 ESP32 仪表盘会自动搜索并连接该 USB 蓝牙接收器发出的空中信号！

---

## ❓ 4. 常见问题与恢复指南 (FAQ)

### Q1: 如果想恢复蓝牙接收器在 Windows 下的正常蓝牙功能，该怎么办？
1. 打开 Windows **“设备管理器”**。
2. 展开 **“通用串行总线设备”**（Universal Serial Bus devices），找到 `CSR8510 A10`。
3. 右键点击并选择 **“卸载设备”**，在弹出的确认框中勾选 **“尝试删除此设备的驱动程序”**。
4. 拔出 USB 蓝牙接收器并重新插入，Windows 会自动重新安装官方默认的蓝牙驱动。

### Q2: 启动时提示 `LIBUSB_ERROR_ACCESS [-3]` 错误？
* **原因**：WinUSB 接口被其他程序占用，或者未以管理员权限运行。
* **解决方法**：使用 `run.bat`（自动提权），或者以管理员身份打开 PowerShell / CMD 运行 `python sim_server.py`。

### Q3: 本电脑既有自带蓝牙又有 USB 适配器，冲突吗？
* 完全不冲突！Windows 自带蓝牙继续服务于 Windows 系统；`sim_server.py` 会精准通过 USB VID/PID 抓取插入的 CSR8510 接收器，实现双卡独立工作。
