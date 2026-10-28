#include <Arduino.h>

#include "app/Dashboard.h"
#include "ble/BleLink.h"
#include "config.h"
#include "data/MockSource.h"
#include "data/VehicleData.h"
#include "display/Display.h"
#include "tesla/TeslaClient.h"

// ---------------------------------------------------------------------------
// 数据流：
//   CFG_DEMO_MODE=1: MockSource(9阶段剧本) --TeslaPayload--> g_vehicle
//   CFG_DEMO_MODE=0: 模拟器/真车 --BLE notify--> BleLink --53B组帧-->
//                    TeslaClient --> g_vehicle
//   两条路径汇合后统一经 applyTesla() 写快照 --> Dashboard(LVGL) 渲染
// ---------------------------------------------------------------------------
BleLink bleLink;
TeslaClient teslaClient;
MockSource mockSource;
Dashboard dashboard;
Display display;

void setup() {
  Serial.begin(115200);
  delay(200);
  CFG_LOG("==============================");
  CFG_LOG(" ESP32 BLE Tesla Dashboard");
  CFG_LOG("==============================");

  display.begin();   // 屏幕 + LVGL 先起
  dashboard.begin(); // UI 对象树

#if CFG_DEMO_MODE
  // 演示模式：内建剧本，不启动 BLE
  mockSource.begin();
#else
  bleLink.begin();

  // 目标广播名：S + sha1(VIN) 前16位hex + C（与 sim_server.py 规则一致）
  String targetName =
      strlen(CFG_TESLA_NAME) > 0
          ? String(CFG_TESLA_NAME)
          : TeslaClient::advertisedNameFromVin(CFG_TESLA_VIN);
  if (targetName.length() == 0) {
    CFG_LOG("广播名推导失败（VIN 为空？），退回前缀/UUID 扫描");
  } else {
    bleLink.setTargetName(targetName);
    CFG_LOG("目标 VIN=%s  广播名=%s", CFG_TESLA_VIN, targetName.c_str());
  }

  bleLink.setRxHandler([](const uint8_t* data, size_t len) {
    teslaClient.feed(data, len);
  });
  teslaClient.begin(&bleLink);
#endif
}

void loop() {
#if CFG_DEMO_MODE
  mockSource.loop();
  dashboard.render(g_vehicle, BleLink::State::Ready, true);
#else
  bleLink.loop();
  teslaClient.loop();
  dashboard.render(g_vehicle, bleLink.state(), teslaClient.ready());
#endif
  display.loop();  // LVGL 事务驱动
  delay(5);
}
