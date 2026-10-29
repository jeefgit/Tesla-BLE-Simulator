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
// 演示模式（屏幕开发期临时用，蓝牙接收器到货后改回 0 即切回真实 BLE 数据流）
//   1 = 固件内建 9 阶段车况剧本（解锁→起步→超车→能量回收→FSD→倒车→开门→充电），
//       不启动 BLE 扫描，屏幕可独立开发调试
//   0 = 正常特斯拉 BLE（扫描 sim_server/真车 → 53B SimPayload）
// ---------------------------------------------------------------------------
#define CFG_DEMO_MODE 0

// 剧本推进间隔（毫秒）。1030 帧 × 间隔 = 一个完整循环
//   800 = 与真车模拟器节奏一致；200 = 快速轮播便于看各屏状态
#define CFG_MOCK_TICK_MS 800

// ---------------------------------------------------------------------------
// BLE 扫描 / 连接
// ---------------------------------------------------------------------------

// 扫描时长（秒）
#define CFG_SCAN_SECONDS 20

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
// 显示：2.8 寸 320×240 SPI（LVGL 9 + LovyanGFX）
// 当前硬件：ESP32-2432S028R（CYD 板）——板载 ILI9341 + XPT2046 触摸，背光 GPIO21
//
// ⚠️ 这块克隆 ILI9341 面板的初始化参数是实测出来的：
//    必须按 320×240 声明 + offset_rotation=4 + setRotation(1)，
//    改成原生 240×320 声明会花屏。所以面板参数不要再动。
//
// 这种初始化下，驱动层坐标是「竖装」240×320，而 UI 设计是 320×240 横屏，
// 因此方向由 Display.cpp 的刷新回调做 90° 旋转（CFG_LCD_FLUSH_ROTATION）解决，
// 面板初始化保持原样 —— 画错方向只会转/翻，不会花屏。
// ---------------------------------------------------------------------------

// 驱动芯片选择：1 = ILI9341（CYD 2432S028 板载），0 = ST7789（独立排线屏常见）
#define CFG_LCD_DRIVER_ILI9341 1

// 面板驱动旋转 / 镜像位：固定 1 / 4（实测稳定组合，改别的值可能花屏）
#define CFG_LCD_ROTATION 1
#define CFG_LCD_OFFSET_ROTATION 4

// 刷新层旋转（不碰面板初始化，只决定 UI 画面的朝向）：
//   90  = 画面顺时针转 90°（默认，横屏）
//   270 = 画面逆时针转 90°（横屏，和 90 差 180°，用来纠正上下颠倒）
//   0   = 不旋转（竖屏，仅调试用）
#define CFG_LCD_FLUSH_ROTATION 90

// 引脚取自出厂演示的 TFT_eSPI 配置（实测可显示）：
// 这块板 LCD 走 HSPI 14/13，18/23 是 TF 卡的脚；RST 与 MISO 共用 IO12
#define CFG_LCD_SCK 14
#define CFG_LCD_MOSI 13
#define CFG_LCD_MISO 12
#define CFG_LCD_CS 15
#define CFG_LCD_DC 2
#define CFG_LCD_RST 12  // 与 MISO 共用 IO12（LovyanGFX 会先拉低复位再切回 SPI 读）
#define CFG_LCD_BL 21   // 背光脚，无背光控制填 -1
// LVGL 逻辑画布：横屏 320×240（UI 设计分辨率，不随面板参数变化）
#define CFG_LCD_WIDTH 320
#define CFG_LCD_HEIGHT 240
#define CFG_LCD_SPI_FREQ 40000000  // SPI 时钟（此板 40MHz 稳定）

// 仪表行打印间隔（毫秒），串口日志限流用
#define CFG_RENDER_INTERVAL_MS 500

// ---------------------------------------------------------------------------
// 调试日志
// ---------------------------------------------------------------------------
#define CFG_LOG(fmt, ...) \
  Serial.printf("[%.2fs] " fmt "\r\n", millis() / 1000.0f, ##__VA_ARGS__)
