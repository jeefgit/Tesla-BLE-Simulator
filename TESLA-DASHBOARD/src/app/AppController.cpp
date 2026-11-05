#include "app/AppController.h"

#include "config.h"

void AppController::begin() {
  // 屏幕和 LVGL 必须先起，UI 才能创建对象树
  m_display.begin(m_board);
  m_ui.begin();
  m_source.begin();
}

void AppController::loop() {
  m_source.loop();

  // 组装只读快照：前端不接触任何后端对象
  m_model.vehicle = m_source.state();
  m_model.status = m_source.status();
  m_model.demoMode = CFG_DEMO_MODE != 0;

  m_ui.render(m_model);
  m_display.loop();
}
