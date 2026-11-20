#pragma once

// 配置模式开关：设备开机时依据 NVS 标志决定本次运行是「配置模式」还是
// 「正常模式」。两种模式互斥，通过重启切换，避免 WiFi AP 与 BLE 同时
// 常驻导致内部 SRAM 耗尽（见 WebConfigServer 与 BleLink）。
//
//   - 配置模式：只开 WiFi 热点 + 网页，供手机 OTA / 改 VIN / 看日志；
//   - 正常模式：只跑 BLE 仪表。
//
// 标志写入由 DashboardApp（BOOT 长按）或 WebConfigServer（网页退出）触发，
// 随后重启生效。
namespace configmode {

// 启动时调用一次：读取 NVS 标志，确定本次运行模式。
void begin();

// 本次运行是否处于配置模式（begin() 之后有效）。
bool active();

// 写入 NVS 标志（仅写，不重启；调用方负责随后 ESP.restart()）。
void enter();
void exit();

}  // namespace configmode
