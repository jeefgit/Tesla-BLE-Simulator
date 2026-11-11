#include <Arduino.h>

#include "app/DashboardApp.h"
#include "config.h"

DashboardApp dashboard;

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("ESP32-S3 round dashboard starting");
  dashboard.begin();
}

void loop() {
  dashboard.loop();
  // 5ms 足够满足 LVGL 33ms 刷新与 20Hz 仪表更新，空转开销比 2ms 更低。
  delay(5);
}
