#include <Arduino.h>

#include "app/AppController.h"
#include "app/Dashboard.h"
#include "config.h"
#include "data/MockSource.h"
#include <TeslaBleBackend.h>
#include "display/Display.h"
#include "platform/BoardSelect.h"

// ---------------------------------------------------------------------------
// 组合根（Composition Root）：只负责「把谁装给谁」，不含业务逻辑。
//
//   CFG_DEMO_MODE=1: MockSource(9阶段剧本) ─┐
//   CFG_DEMO_MODE=0: TeslaBleSource(BLE)  ─┴─> AppController ─> Dashboard(LVGL)
//                                                              └> Display(LovyanGFX)
//
// 换板/换屏只改 BoardSelect.h 与 platformio.ini；
// 换数据源只改下面的 #if 分支（编译期选择）。
// ---------------------------------------------------------------------------
#if CFG_DEMO_MODE
MockSource vehicleSource;
#else
TeslaBleBackend vehicleSource;
#endif

Dashboard dashboard;
Display display;
AppController app(vehicleSource, dashboard, display, board::kDefault);

void setup() {
  Serial.begin(115200);
  delay(200);
  CFG_LOG("==============================");
  CFG_LOG(" ESP32 BLE Tesla Dashboard");
  CFG_LOG("==============================");

  app.begin();
}

void loop() {
  app.loop();
  // 之前 delay(50) 会把 LVGL 刷新压到 ~20fps，加剧闪烁。
  // 收紧到 2ms：LVGL 自行按 33ms 节拍刷新，画面更平滑；BLE 轮询本就非阻塞。
  delay(2);
}
