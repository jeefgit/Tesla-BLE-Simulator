#pragma once

#include "core/UiModel.h"
#include "data/IVehicleSource.h"
#include "platform/IDisplay.h"
#include "ui/IDashboard.h"

// ---------------------------------------------------------------------------
// AppController：应用装配与主循环
//
// 唯一知道「数据源 + UI + 屏幕」三者的地方：
//   1. begin(): display.begin(board) -> ui.begin() -> source.begin()
//   2. loop():  source.loop() -> 组装 UiModel -> ui.render() -> display.loop()
//
// 三条依赖都是接口，换板/换屏/换数据源都不需要改这里。
// ---------------------------------------------------------------------------
class AppController {
 public:
  AppController(IVehicleSource& source, IDashboard& ui, IDisplay& display,
                const BoardProfile& board)
      : m_source(source), m_ui(ui), m_display(display), m_board(board) {}

  void begin();
  void loop();

 private:
  IVehicleSource& m_source;
  IDashboard& m_ui;
  IDisplay& m_display;
  const BoardProfile& m_board;
  UiModel m_model;
};
