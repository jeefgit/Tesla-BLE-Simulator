"""配钥授权 UI 离屏自测：横幅 + 自动弹窗 + 批准/关闭。"""
import sys, os, queue, threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import tkinter as tk

import sim_server


def main():
    log_queue = queue.Queue()
    state = sim_server.TelemetryState()
    stop_event = threading.Event()
    root = tk.Tk()
    root.geometry("1120x820+40+40")
    gui = sim_server.TeslaSimulatorGUI(root, state, stop_event, log_queue)
    root.update_idletasks()
    root.update()

    assert gui.pair_banner.winfo_manager() == "pack", "配钥横幅未布局"
    assert gui.pair_banner.winfo_ismapped() == 1, "配钥横幅未显示"
    assert gui.btn_pair_approve.winfo_ismapped() == 1, "批准按钮未显示"
    assert str(gui.btn_pair_approve["state"]) == "disabled"
    print("banner idle text:", gui.lbl_pair_banner["text"])

    # 1) 有待授权请求 -> 横幅高亮 + 自动弹窗
    state.set_pair_pending_desc("pub=048fa445f6d89310… role=OWNER")
    gui.periodic_gui_update_tick()
    root.update()
    assert str(gui.btn_pair_approve["state"]) == "normal", "批准按钮未启用"
    assert gui.lbl_pair_banner["fg"] == gui.COLOR_RED
    assert gui._pair_dialog_alive(), "授权弹窗未创建"
    print("banner pending text:", gui.lbl_pair_banner["text"])
    print("dialog alive:", gui._pair_dialog_alive())

    # 2) 点击批准 -> 后台请求被消费
    gui.tap_nfc_card()
    gui.periodic_gui_update_tick()
    root.update()
    approve, reject = state.consume_pair_requests()
    assert approve and not reject, f"批准请求未被消费: {(approve, reject)}"
    print("approve consumed:", (approve, reject))

    # 3) 后台处理完 pending 清空 -> 自动关窗 + 横幅复原
    state.set_pair_pending_desc(None)
    gui.periodic_gui_update_tick()
    root.update()
    assert not gui._pair_dialog_alive(), "授权弹窗未自动关闭"
    assert str(gui.btn_pair_approve["state"]) == "disabled"
    print("dialog alive after clear:", gui._pair_dialog_alive())
    print("GUI PAIR TEST PASSED")
    root.destroy()


if __name__ == "__main__":
    main()
