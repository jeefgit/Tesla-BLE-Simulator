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

// 1 = 先完成加密握手，再用 GetVehicleData 轮询
// 0 = 只接收 53B 旁路 SimPayload（模拟器调试用）
#define CFG_TESLA_CRYPTO 1

// 1 = 辅助状态轮询包含四轮胎压
// 0 = 暂不查询胎压（少发一条 GetVehicleData，仪表胎压显示为断连/占位）
#define CFG_TESLA_QUERY_TIRE_PRESSURE 0

// 1 = 辅助状态轮询包含车门/中控锁状态
// 0 = 暂不查询车门（仪表车门/锁车状态保持断连/占位）
#define CFG_TESLA_QUERY_CLOSURES 0

// 本地真实私钥（被 .gitignore 忽略，不提交）；缺失时用后端内置测试密钥。
#if defined(__has_include)
#if __has_include("secrets/tesla_client_key.h")
#include "secrets/tesla_client_key.h"
#endif
#endif

// 后端诊断日志；Drive 请求耗时最多记录前 10 次响应。
#define CFG_DEBUG_LOG 1
#if CFG_DEBUG_LOG
#define CFG_LOG(fmt, ...) \
  Serial.printf("[%.2fs] " fmt "\r\n", millis() / 1000.0f, ##__VA_ARGS__)
#define CFG_LOG_ALWAYS(fmt, ...) \
  Serial.printf("[%.2fs] " fmt "\r\n", millis() / 1000.0f, ##__VA_ARGS__)
#endif
