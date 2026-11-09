#include <Arduino.h>

#include "config.h"

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("ESP32-S3 round-display firmware scaffold");
  Serial.println("Confirm the exact board model before configuring display hardware.");
}

void loop() {
  delay(10);
}
