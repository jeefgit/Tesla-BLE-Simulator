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

// 车辆识别号：模拟器启动参数 --vin 必须与此一致（默认值已按本车 VIN 配置）
#define CFG_TESLA_VIN "LRWYGCFJ8RC571213"

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
// 特斯拉加密协议（NIST P-256 ECDH + AES-128-GCM + HMAC-SHA256）
//
//   1 = 连接后先握手，再用加密 GetVehicleData 轮询（真车/模拟器加密通道）
//   0 = 只接收 53B 旁路 SimPayload（旧行为，便于对照排查）
// ---------------------------------------------------------------------------
#define CFG_TESLA_CRYPTO 1

// 客户端私钥（64 位 hex，32 字节）。
// 本地真实密钥放在 include/secrets/tesla_client_key.h（已被 .gitignore 忽略），
// 存在时自动覆盖下面的默认值；默认值取自 Tesla 官方 vehicle-command
// 文档公开的调试测试私钥（与模拟器 tesla_protocol/identity.py 的
// TEST_CLIENT_PRIVATE_KEY 一致），仅用于联调/向量比对。
#if defined(__has_include)
#if __has_include("secrets/tesla_client_key.h")
#include "secrets/tesla_client_key.h"
#endif
#endif

#ifndef CFG_TESLA_CLIENT_KEY_HEX
#define CFG_TESLA_CLIENT_KEY_HEX "2538CDC29A97C19C1E99A637D6CF4F8C970C118B56EDE1E6323E6D162C4B30DB"
#endif

// 握手重试间隔（毫秒）与连续失败次数；超过后降级为 53B 旁路并周期性重试
#define CFG_TESLA_HANDSHAKE_RETRY_MS 2000
#define CFG_TESLA_HANDSHAKE_MAX_RETRY 3
#define CFG_TESLA_HANDSHAKE_RETRY_LONG_MS 15000

// GetVehicleData 轮询间隔（毫秒）
#define CFG_TESLA_POLL_INTERVAL_MS 1500

// ---------------------------------------------------------------------------
// BLE 配钥（add-key-request，对齐官方 vehicle-command cmd/tesla-control）
//
// 车辆握手回 SessionInfo.status = KEY_NOT_ON_WHITELIST 时，说明本机公钥
// 还没登记。此时按官方 add-key-request 流程发送「明文 ToVCSECMessage +
// SIGNATURE_TYPE_PRESENT_KEY」，车辆会在中控台提示「轻点 NFC 钥匙卡」，
// 刷卡确认后回 WhitelistOperation_status 终态，之后重新握手即可。
//
//   1 = 握手被拒时自动发起配钥请求
//   0 = 不自动配钥（公钥未登记时只打印提示，需自己在车机/模拟器授权）
// ---------------------------------------------------------------------------
#define CFG_TESLA_PAIR_ON_DENIED 1

// 配钥请求重发间隔（毫秒）与最多发送次数。
// 车辆回 WAIT 后进入纯等待刷卡，不再重发（重复请求可能刷新/打断车端提示）；
// 真车从休眠被 BLE 唤醒需要几秒，未收到 WAIT 时按此间隔补发，最多 3 次。
#define CFG_TESLA_PAIR_RETRY_MS 8000
#define CFG_TESLA_PAIR_MAX_SENDS 3

// 等待刷卡超时（毫秒）：超时后清空发送计数并重新发起 add-key-request，
// 避免车端提示已消失/超时后固件永久停在「只等不重试」的状态。
#define CFG_TESLA_PAIR_WAIT_TIMEOUT_MS 90000

// 握手连续多次仍未建立会话时，直接按官方做法发 add-key-request
// （真车对未登记公钥的 SessionInfoRequest 可能不回包，也可能只回无法解析的通知；
//  add-key-request 本身不依赖握手，不能因为「收到过字节」就放弃配钥）
// 4 次 × 2s ≈ 8s：已配对且车已唤醒时首次握手就会回包，不会误触发配钥提示
#define CFG_TESLA_PAIR_FALLBACK_TRIES 4

// 申请的角色与钥匙形态（Keys.Role: 2=OWNER 3=DRIVER；
// KeyFormFactor: 7=ANDROID_DEVICE 9=CLOUD_KEY，官方 BLE 配钥示例用 9）
#define CFG_TESLA_PAIR_ROLE 2
#define CFG_TESLA_PAIR_FORM_FACTOR 9

// 命令有效期（秒）。车端允许 15s 时钟偏差，60s 足够覆盖轮询窗口
#define CFG_TESLA_COMMAND_TTL_S 60

// 加密帧上限（协议长度前缀最大 4096，实际 GetVehicleData 响应 ~200B）
#define CFG_TESLA_MAX_FRAME 2048

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

// 颜色反相：1 = 面板反色，0 = 正常
// 现象判断：UI 背景设计是纯黑，如果屏幕上是白色 → 置 0；反之若是黑白颠倒的屏 → 置 1
#define CFG_LCD_INVERT 0

// 红蓝通道顺序：1 = RGB（正常），0 = BGR（红蓝互换）
// LovyanGFX 内部：madctl |= rgb_order ? MAD_RGB : MAD_BGR（默认 0 会走 BGR）
// 现象判断：纯黑/纯白不受影响；若暗蓝色块（车身底 C_TRACK #0E1219、槽体）看起来发红，
//           或红色告警显示成蓝色 → 与当前值取反即可
#define CFG_LCD_RGB_ORDER 1

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
