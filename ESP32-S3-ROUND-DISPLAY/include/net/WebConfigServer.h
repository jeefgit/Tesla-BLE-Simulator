#pragma once

#include <stdint.h>

#include "config.h"

// 设备自建 WiFi 热点 + HTTP 网页，供手机进行配置管理（OTA / VIN / 日志）。
// 实现见 src/net/WebConfigServer.cpp。
class WebConfigServer {
 public:
  // 在显示 / UI 初始化之后调用：开启 AP 并注册路由。
  void begin();

  // 主循环调用：处理 HTTP 请求。返回 true 表示有客户端连接（或刚处理完
  // 请求），调用方可借此重置自动休眠计时。
  bool loop();

  // 当前热点名称与 IP（begin() 后有效；AP 启动失败时保持 "--"）。
  // 供仪表盘在配置模式页面上显示。
  const char* ssid() const { return m_ssid; }
  const char* ip() const { return m_ip; }

 private:
  void registerRoutes();

  void handleRoot();
  void handleStatus();
  void handleVinGet();
  void handleVinSet();
  void handleLogList();
  void handleLogRead();
  void handleLogClear();
  void handleReboot();
  void handleConfigExit();
  void handleOtaFinish();
  void handleOtaUpload();

  bool m_started = false;
  bool m_otaError = false;
  char m_otaErrorText[64] = {};
  char m_ssid[33] = "--";
  char m_ip[16] = "--";
  // 成功写入 VIN / 升级后延时重启的时刻；0 = 无待重启。
  uint32_t m_restartAt = 0;
};
