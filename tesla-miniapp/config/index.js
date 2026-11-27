// 全局配置：协议常量 + 与 TeslaBleBackend BackendConfig.h 对齐的默认参数
'use strict';

const UUID = {
  SERVICE: '00000211-b2d1-43f0-9b88-960cebf8b91e',
  WRITE: '00000212-b2d1-43f0-9b88-960cebf8b91e',
  NOTIFY: '00000213-b2d1-43f0-9b88-960cebf8b91e',
};

// 与 ESP32 端 BackendConfig.h 对齐
const CFG = {
  VIN: 'LRWYGCFJ8RC571213', // 出厂默认 VIN，可被设置页存储值覆盖
  // 手动指定的广播名（留空则只用 VIN 推导名）。真车若广播的是车机自定义名称，
  // 填这里可以让界面把它标成“VIN 匹配”；不填也能在设备列表里手动选择。
  BLE_NAME: '',
  DATA_STALE_MS: 3000,
  SCAN_SECONDS: 1,
  RECONNECT_INTERVAL_MS: 3000,
  CONNECT_TIMEOUT_MS: 10000,
  MTU_TARGET: 247, // setBLEMTU 目标值，失败回退 20B 分片
  // 加密握手
  HANDSHAKE_RETRY_MS: 2000,
  HANDSHAKE_MAX_RETRY: 3,
  HANDSHAKE_RETRY_LONG_MS: 15000,
  // 数据轮询
  DRIVE_POLL_INTERVAL_MS: 300,
  AUX_POLL_INTERVAL_MS: 30000, // 辅助数据（续航/温度/胎压等）每 30s 整批重新抓取
  AUX_DATA_STALE_MS: 126000,
  POLL_RESPONSE_TIMEOUT_MS: 1500,
  POLL_RETRY_MS: 500,
  // 配钥
  PAIR_ON_DENIED: true,
  PAIR_RETRY_MS: 8000,
  PAIR_MAX_SENDS: 3,
  PAIR_TAP_WINDOW_MS: 30000,
  PAIR_WAIT_TIMEOUT_MS: 90000,
  PAIR_ROLE: 2,
  PAIR_FORM_FACTOR: 9,
  COMMAND_TTL_S: 60,
  MAX_FRAME: 2048,
};

// 53B SimPayload（TeslaPayload.h）
const SIM_PAYLOAD_LEN = 53;

module.exports = { UUID, CFG, SIM_PAYLOAD_LEN };
