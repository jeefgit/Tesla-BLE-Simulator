import sys
import winreg

def restore():
    # 尝试获取当前系统的蓝牙适配器 ID
    try:
        from bless.backends.winrt.ble.adapter import get_bluetooth_adapter
        adapter_id = get_bluetooth_adapter()
    except ImportError:
        adapter_id = ""

    if not adapter_id:
        print("[-] 未检测到活动的 USB 蓝牙适配器，无法定位注册表路径。")
        print("[-] 请确保您的 USB 蓝牙适配器已插入电脑。")
        return

    path = f"SYSTEM\\ControlSet001\\Enum\\{adapter_id}\\Device Parameters"
    print(f"[+] 正在检测注册表路径: HKLM\\{path}")

    try:
        # 打开注册表键并删除 Local Name 覆写值
        key = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, path, 0, winreg.KEY_SET_VALUE)
        winreg.DeleteValue(key, "Local Name")
        winreg.CloseKey(key)
        print("[+] 成功！已从注册表中清除 'Local Name' 覆写字段。")
        print("[+] 恢复生效步骤：")
        print("    1. 请拔掉您的 USB 蓝牙接收器。")
        print("    2. 等待 2 秒后重新插回。")
        print("    3. 此时您的蓝牙被发现名称将彻底跟随并恢复为您的 Windows 系统电脑名。")
    except FileNotFoundError:
        print("[+] 注册表中未发现 'Local Name' 覆写字段，您的蓝牙适配器已是默认状态。")
    except PermissionError:
        print("[-] 错误: 权限不足。请以【管理员身份】运行此脚本以清理注册表！")
    except Exception as e:
        print(f"[-] 恢复失败: {e}")

if __name__ == '__main__':
    restore()
