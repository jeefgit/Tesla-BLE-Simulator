#include <Arduino.h>

#include "app/Dashboard.h"
#include "ble/BleLink.h"
#include "config.h"
#include "data/VehicleData.h"
#include "tesla/TeslaClient.h"

// ---------------------------------------------------------------------------
// 数据流（只走特斯拉 BLE，无 OBD）：
//   模拟器/真车 --BLE notify(800ms 节拍)--> BleLink --字节-->
//   TeslaClient --53B组帧/解析--> g_vehicle --> Dashboard
// ---------------------------------------------------------------------------
BleLink bleLink;
TeslaClient teslaClient;
Dashboard dashboard;

void setup() {
  Serial.begin(115200);
  delay(200);
  CFG_LOG("==============================");
  CFG_LOG(" ESP32 BLE Tesla Dashboard");
  CFG_LOG("==============================");

  dashboard.begin();
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
}

void loop() {
  bleLink.loop();
  teslaClient.loop();
  dashboard.render(g_vehicle, bleLink.state(), teslaClient.ready());
  delay(5);
}
