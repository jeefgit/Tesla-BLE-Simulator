#pragma once

#if defined(__has_include)
#if __has_include(<tesla_backend_user_config.h>)
#include <tesla_backend_user_config.h>
#endif
#endif

#ifndef CFG_TESLA_UNITS_MILES
#define CFG_TESLA_UNITS_MILES 1
#endif
#ifndef CFG_MILES_TO_KM
#define CFG_MILES_TO_KM 1.609344f
#endif
#ifndef CFG_TESLA_VIN
#define CFG_TESLA_VIN ""
#endif
#ifndef CFG_TESLA_NAME
#define CFG_TESLA_NAME ""
#endif
#ifndef CFG_TESLA_PAYLOAD_LEN
#define CFG_TESLA_PAYLOAD_LEN 53
#endif
#ifndef CFG_DATA_STALE_MS
#define CFG_DATA_STALE_MS 3000
#endif
#ifndef CFG_SCAN_SECONDS
#define CFG_SCAN_SECONDS 3
#endif
#ifndef CFG_RECONNECT_INTERVAL_MS
#define CFG_RECONNECT_INTERVAL_MS 3000
#endif
#ifndef CFG_CONNECT_TIMEOUT_MS
#define CFG_CONNECT_TIMEOUT_MS 10000
#endif
#ifndef CFG_CONNECT_FAIL_LIMIT
#define CFG_CONNECT_FAIL_LIMIT 3
#endif
#ifndef CFG_MTU
#define CFG_MTU 515
#endif
#ifndef CFG_TARGET_NAMES
#define CFG_TARGET_NAMES {"S"}
#endif
#ifndef CFG_SERVICE_UUID
#define CFG_SERVICE_UUID "00000211-b2d1-43f0-9b88-960cebf8b91e"
#endif
#ifndef CFG_CHAR_WRITE_UUID
#define CFG_CHAR_WRITE_UUID "00000212-b2d1-43f0-9b88-960cebf8b91e"
#endif
#ifndef CFG_CHAR_NOTIFY_UUID
#define CFG_CHAR_NOTIFY_UUID "00000213-b2d1-43f0-9b88-960cebf8b91e"
#endif
#ifndef CFG_TESLA_CRYPTO
#define CFG_TESLA_CRYPTO 1
#endif
#ifndef CFG_TESLA_CLIENT_KEY_HEX
#define CFG_TESLA_CLIENT_KEY_HEX \
  "2538CDC29A97C19C1E99A637D6CF4F8C970C118B56EDE1E6323E6D162C4B30DB"
#endif
#ifndef CFG_TESLA_HANDSHAKE_RETRY_MS
#define CFG_TESLA_HANDSHAKE_RETRY_MS 2000
#endif
#ifndef CFG_TESLA_HANDSHAKE_MAX_RETRY
#define CFG_TESLA_HANDSHAKE_MAX_RETRY 3
#endif
#ifndef CFG_TESLA_HANDSHAKE_RETRY_LONG_MS
#define CFG_TESLA_HANDSHAKE_RETRY_LONG_MS 15000
#endif
#ifndef CFG_TESLA_DRIVE_POLL_INTERVAL_MS
#define CFG_TESLA_DRIVE_POLL_INTERVAL_MS 300
#endif
#ifndef CFG_TESLA_AUX_POLL_INTERVAL_MS
#define CFG_TESLA_AUX_POLL_INTERVAL_MS 31000
#endif
// 是否在辅助状态轮询中查询四轮胎压（GetVehicleData: TirePressure）。
//   1 = 每轮辅助状态都请求胎压
//   0 = 跳过胎压请求（后台不再发胎压命令，仪表胎压保持断连/占位显示）
#ifndef CFG_TESLA_QUERY_TIRE_PRESSURE
#define CFG_TESLA_QUERY_TIRE_PRESSURE 1
#endif
// 是否在辅助状态轮询中查询车门/中控锁状态（GetVehicleData: Closures）。
//   1 = 每轮辅助状态都请求车门状态
//   0 = 跳过车门状态请求（仪表车门/锁车显示保持断连/占位）
#ifndef CFG_TESLA_QUERY_CLOSURES
#define CFG_TESLA_QUERY_CLOSURES 1
#endif
#ifndef CFG_TESLA_POLL_RETRY_MS
#define CFG_TESLA_POLL_RETRY_MS 500
#endif
#ifndef CFG_TESLA_POLL_RESPONSE_TIMEOUT_MS
#define CFG_TESLA_POLL_RESPONSE_TIMEOUT_MS 1500
#endif
#ifndef CFG_TESLA_MTU_FAULT_LOG_EVERY
#define CFG_TESLA_MTU_FAULT_LOG_EVERY 30
#endif
#ifndef CFG_TESLA_PAIR_ON_DENIED
#define CFG_TESLA_PAIR_ON_DENIED 1
#endif
#ifndef CFG_TESLA_PAIR_RETRY_MS
#define CFG_TESLA_PAIR_RETRY_MS 8000
#endif
#ifndef CFG_TESLA_PAIR_MAX_SENDS
#define CFG_TESLA_PAIR_MAX_SENDS 3
#endif
#ifndef CFG_TESLA_PAIR_TAP_WINDOW_MS
#define CFG_TESLA_PAIR_TAP_WINDOW_MS 30000
#endif
#ifndef CFG_TESLA_PAIR_WAIT_TIMEOUT_MS
#define CFG_TESLA_PAIR_WAIT_TIMEOUT_MS 90000
#endif
#ifndef CFG_TESLA_PAIR_FALLBACK_TRIES
#define CFG_TESLA_PAIR_FALLBACK_TRIES 4
#endif
#ifndef CFG_TESLA_PAIR_ROLE
#define CFG_TESLA_PAIR_ROLE 2
#endif
#ifndef CFG_TESLA_PAIR_FORM_FACTOR
#define CFG_TESLA_PAIR_FORM_FACTOR 9
#endif
#ifndef CFG_TESLA_COMMAND_TTL_S
#define CFG_TESLA_COMMAND_TTL_S 60
#endif
#ifndef CFG_TESLA_MAX_FRAME
#define CFG_TESLA_MAX_FRAME 2048
#endif
#ifndef CFG_DEBUG_LOG
#define CFG_DEBUG_LOG 0
#endif
#ifndef CFG_LOG
#define CFG_LOG(...) \
  do {                \
  } while (0)
#endif
#ifndef CFG_LOG_ALWAYS
#define CFG_LOG_ALWAYS(...) \
  do {                       \
  } while (0)
#endif
