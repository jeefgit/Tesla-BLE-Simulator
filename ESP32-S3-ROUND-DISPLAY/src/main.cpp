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

void loop() { dashboard.loop(); }
