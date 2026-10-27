#pragma once
//
// ESP32 BLE Tesla Dashboard —— 全局配置
// 所有可调参数集中在这里，业务代码里不写魔法数字。
//
// 数据源：只走特斯拉 BLE（模拟器/真车），OBD 路径不再使用
// （src/obd 已通过 platformio.ini 的 build_src_filter 排除编译）。
//

#include <Arduino.h>

// ---------------------------------------------------------------------------
// 目标车辆
// ---------------------------------------------------------------------------

// 车辆识别号：模拟器启动参数 --vin 必须与此一致
#define CFG_TESLA_VIN "5YJ3E1EA0KF000000"

// 广播名留空 = 由 VIN 自动推导：S + sha1(VIN) 前16位hex + C
// （与仓库根目录 sim_server.py 规则一致）。模拟器用别的 --vin 时可直接写死名字。
#define CFG_TESLA_NAME ""

// SimPayload 帧长（struct.pack("<ffifc6BBfff4f")，见根 README 协议规范）
#define CFG_TESLA_PAYLOAD_LEN 53

// 数据超时：超过该毫秒数没收到新帧，仪表把字段降级显示为 --
#define CFG_DATA_STALE_MS 3000

// ---------------------------------------------------------------------------
// BLE 扫描 / 连接
// ---------------------------------------------------------------------------

// 扫描时长（秒）
#define CFG_SCAN_SECONDS 10

// 断线后重连间隔（毫秒）
#define CFG_RECONNECT_INTERVAL_MS 3000

// 连续连接失败多少次后放弃该地址、重新扫描
#define CFG_CONNECT_FAIL_LIMIT 3

// 连接后请求的 MTU（SimPayload 53 字节依赖大 MTU 一次推完）
#define CFG_MTU 512

// 名称前缀兜底名单：广播名按 VIN 精确匹配（setTargetName），
// 只有未配置精确名时才退回前缀扫描，留空则完全依赖精确名 + 服务 UUID
#define CFG_TARGET_NAMES {"S"}

// ---------------------------------------------------------------------------
// 特斯拉 BLE GATT（docs/HANDOVER.md §UUID，与 sim_server.py 一致）
//   Service  00000211  Write 00000212（ESP32 → 车）  Notify 00000213（车 → ESP32）
// ---------------------------------------------------------------------------
#define CFG_SERVICE_UUID    "00000211-b2d1-43f0-9b88-960cebf8b91e"
#define CFG_CHAR_WRITE_UUID "00000212-b2d1-43f0-9b88-960cebf8b91e"
#define CFG_CHAR_NOTIFY_UUID "00000213-b2d1-43f0-9b88-960cebf8b91e"

// ---------------------------------------------------------------------------
// 显示
// ---------------------------------------------------------------------------

// 仪表刷新间隔（毫秒），接屏前先用串口模拟
#define CFG_RENDER_INTERVAL_MS 500

// ---------------------------------------------------------------------------
// 调试日志
// ---------------------------------------------------------------------------
#define CFG_LOG(fmt, ...) \
  Serial.printf("[%.2fs] " fmt "\r\n", millis() / 1000.0f, ##__VA_ARGS__)
