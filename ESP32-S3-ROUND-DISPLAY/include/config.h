#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// 演示 / 真实数据
//   1 = 保留 UiModel 的静态示例值（无车联调 UI）
//   0 = 启动 TeslaBleBackend，扫描并连接车辆
// ---------------------------------------------------------------------------
#define CFG_DEMO_MODE 0
#define CFG_LOG_ENABLED 1

// ---------------------------------------------------------------------------
// TeslaBleBackend 配置（tesla_backend_user_config.h 会包含本文件）
// ---------------------------------------------------------------------------
#define CFG_TESLA_VIN "LRWYGCFJ8RC571213"

// 留空 = 由 VIN 推导广播名：S + sha1(VIN) 前 16 位 hex + C
#define CFG_TESLA_NAME ""

// 1 = 车端原始值为英里，解析时换算成公里（真车）
#define CFG_TESLA_UNITS_MILES 1

// 驾驶状态轮询间隔；单条请求仍需等车端响应后才会发送下一条。
#define CFG_TESLA_DRIVE_POLL_INTERVAL_MS 280

#define CFG_TESLA_AUX_POLL_INTERVAL_MS 500

// 1 = 先完成加密握手，再用 GetVehicleData 轮询
// 0 = 只接收 53B 旁路 SimPayload（模拟器调试用）
#define CFG_TESLA_CRYPTO 1

// 1 = 辅助状态轮询包含四轮胎压
// 0 = 暂不查询胎压（少发一条 GetVehicleData，仪表胎压显示为断连/占位）
#define CFG_TESLA_QUERY_TIRE_PRESSURE 0

// 1 = 辅助状态轮询包含车门/中控锁状态
// 0 = 暂不查询车门（仪表车门/锁车状态保持断连/占位）
#define CFG_TESLA_QUERY_CLOSURES 1

// 本地真实私钥（被 .gitignore 忽略，不提交）；缺失时用后端内置测试密钥。
#if defined(__has_include)
#if __has_include("secrets/tesla_client_key.h")
#include "secrets/tesla_client_key.h"
#endif
#endif

// ---------------------------------------------------------------------------
// 自动休眠 / 运动唤醒
//   设备（板子自身）持续静止后熄屏并进入 ESP32-S3 deep sleep；由 QMI8658
//   的 Wake-on-Motion 通过 INT2（GPIO21）唤醒。判断只使用 IMU，不读取车速。
// ---------------------------------------------------------------------------
#define CFG_AUTO_SLEEP_ENABLE 1

// 静止多久后进入 deep sleep；默认 60 秒。
#define CFG_AUTO_SLEEP_IDLE_MS (150UL * 1000UL)

// 是否使用 QMI8658 检测板子运动。
#define CFG_AUTO_SLEEP_IMU_ENABLE 1

// 运行期 IMU 轮询间隔；25~50 Hz 足够。
#define CFG_AUTO_SLEEP_IMU_POLL_MS 40

// 运动判定阈值（mg，1 mg = 0.001 g）；先 40~80，再按实测调整。
#define CFG_AUTO_SLEEP_MOTION_THRESHOLD_MG 50

// 连续多少个采样超过阈值才认为“真的在动”。
#define CFG_AUTO_SLEEP_MOTION_CONFIRM_SAMPLES 3

// 充电中是否允许休眠（1 = 允许）。
// 本机没有电池、长期插 USB 供电，所以 USB 插入 / 车辆充电都不应阻止休眠。
// 如果以后换成带电池的版本、希望充电时保持亮屏，再改为 0。
#define CFG_AUTO_SLEEP_WHILE_CHARGING 1

// 运动唤醒引脚：QMI8658 INT2 = GPIO21（属于 ESP32-S3 RTC GPIO）。
#define CFG_AUTO_SLEEP_WAKE_GPIO 21

// WoM 阈值（mg）；越大越不敏感、越省电，但越容易漏掉轻微运动。
#define CFG_AUTO_SLEEP_WOM_THRESHOLD_MG 50

// WoM blanking 样本数，避免刚开启 WoM 时的瞬态误触发。
#define CFG_AUTO_SLEEP_WOM_BLANKING 8

// 唤醒后确认运动的时间；0 = 不确认。
// 本机长期 USB 供电，唤醒后不应很快又睡回去（否则拿起来会看到界面一闪），
// 所以默认关闭。需要防“碰一下就唤醒”时可改成 2000。
#define CFG_AUTO_SLEEP_WAKE_CONFIRM_MS 0

// 可选：定时唤醒兜底（0 = 关闭）；单位 ms。
#define CFG_AUTO_SLEEP_TIMER_WAKE_MS 0

// 后端诊断日志；Drive 请求耗时最多记录前 10 次响应。
#define CFG_DEBUG_LOG 1
#if CFG_DEBUG_LOG
#define CFG_LOG(fmt, ...) \
  Serial.printf("[%.2fs] " fmt "\r\n", millis() / 1000.0f, ##__VA_ARGS__)
#define CFG_LOG_ALWAYS(fmt, ...) \
  Serial.printf("[%.2fs] " fmt "\r\n", millis() / 1000.0f, ##__VA_ARGS__)
#endif

// ---------------------------------------------------------------------------
// 离线小精灵（design/ELF-DESIGN.md）
//   纯本地浮层：仪表任意页左滑呼出、右滑退出，不联网、不出声。
// ---------------------------------------------------------------------------
#define CFG_ELF_ENABLE 1             // 0 = 完全关闭（不建对象、不响应手势）
#define CFG_ELF_SWIPE_PX 60          // 横向滑动触发距离
#define CFG_ELF_DOMINANCE_PX 16      // 横向需领先纵向的幅度（与亮度手势共用）
#define CFG_ELF_TAP_PX 12            // 位移小于此值算轻点
#define CFG_ELF_AUTO_HIDE_MS 0UL     // 无交互自动隐藏；0 = 常亮不自动退出
#define CFG_ELF_SLEEP_MS 20000UL     // 进入困倦
#define CFG_ELF_SLEEP_DEEP_MS 30000UL
#define CFG_ELF_BLINK_MIN_MS 2400UL
#define CFG_ELF_BLINK_MAX_MS 6000UL
#define CFG_ELF_GAZE_MIN_MS 2500UL
#define CFG_ELF_GAZE_MAX_MS 5000UL

// ---------------------------------------------------------------------------
// 阶段 5：声音律动（design/ELF-DESIGN.md §5.6）
//   只收不放：小精灵可见时用 ES7210 双麦感知音量/节拍，不录音、不上传。
// ---------------------------------------------------------------------------
#define CFG_ELF_SOUND_ENABLE 1           // 0 = 不初始化麦克风，完全沿用阶段 1–4
// 1 = 模拟声音输入：不碰 ES7210/I2S/Wire，按时间生成
//     静音 4s -> 环境声 4s -> 120BPM 律动 12s 的循环，方便无麦时检查小精灵联动。
// 0 = 使用真实麦克风采集。
#define CFG_ELF_SOUND_SIMULATE 0
#define CFG_ELF_SOUND_SAMPLE_RATE 16000  // ES7210 采样率
#define CFG_ELF_SOUND_HOP_MS 16          // 特征帧移（分析窗口 32ms）
#define CFG_ELF_SOUND_NOISE_GATE 8       // level 低于此值视为静音（0–255）
#define CFG_ELF_SOUND_QUIET_MS 2000UL    // 静音多久回 Quiet
#define CFG_ELF_SOUND_BEAT_MIN_BPM 60    // 有效节拍下限
#define CFG_ELF_SOUND_BEAT_MAX_BPM 200   // 有效节拍上限
#define CFG_ELF_SOUND_BEAT_REFRACTORY_MS 200UL  // 两拍最小间隔
#define CFG_ELF_SOUND_BEAT_CONFIRM 2     // 1.2s 内至少几拍才进 Grooving
#define CFG_ELF_SOUND_MIN_CONFIDENCE 80  // 进入 Grooving 的最低节拍置信度（0–255）
#define CFG_ELF_SOUND_ONSET_THRESHOLD 0.45f  // 节拍起音阈值；越低越灵敏
#define CFG_ELF_SOUND_GROOVE_HOLD_MS 1600UL     // 无拍后保持律动的时间
#define CFG_ELF_SOUND_BOUNCE_PX 20       // 弹跳最大行程
#define CFG_ELF_SOUND_SWAY_PX 12         // 摇摆最大行程
#define CFG_ELF_SOUND_LEVEL_GAIN 6       // ES7210 输入增益档位（比原设置提高约 6dB）
#define CFG_ELF_SOUND_LOG 1              // 1 = 每 500ms 打印 level/bpm/conf
