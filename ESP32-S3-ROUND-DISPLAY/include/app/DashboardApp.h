#pragma once

#include "config.h"
#include "core/UiModel.h"
#include "input/Cst9217Touch.h"
#include "input/Qmi8658.h"
#include "power/PowerManager.h"
#include "ui/ElfOverlay.h"

#if !CFG_DEMO_MODE
#include "data/TeslaUiSource.h"
#endif

class DashboardApp {
 public:
  void begin();
  void loop();
  void setModel(const UiModel& model);

 private:
  void pollBootButton();
  void beginPowerButton();
  void pollPowerButton();
  void beginTouch();
  void pollTouch();

#if !CFG_DEMO_MODE
  TeslaUiSource m_source;
  bool m_sourceStarted = false;
#endif
  bool m_bootRawPressed = false;
  bool m_bootPressed = false;
  uint32_t m_bootLastChangeMs = 0;
  // 触摸滑动手势状态（坐标均已换算成 LVGL 画面坐标）。
  Cst9217Touch m_touch;
  bool m_touchDown = false;
  bool m_touchHorizontal = false;
  bool m_swipeActive = false;
  bool m_elfSwipeLatched = false;
  bool m_elfWasVisible = false;
  int16_t m_touchStartX = 0;
  int16_t m_touchStartY = 0;
  int16_t m_touchStartRawX = 0;
  int16_t m_touchStartRawY = 0;
  int16_t m_touchLastX = 0;
  int16_t m_touchLastY = 0;
  int16_t m_swipeAnchorY = 0;
  uint8_t m_swipeBaseBrightness = 0;
  bool m_pwrAvailable = false;
  bool m_pwrArmed = false;
  bool m_pwrRawPressed = false;
  bool m_pwrPressed = false;
  uint32_t m_pwrLastChangeMs = 0;
  uint32_t m_pwrPressStartMs = 0;
  Qmi8658 m_imu;
  PowerManager m_power;
  UiModel m_model;
  bool m_initialized = false;
  uint32_t m_lastRenderMs = 0;
  UiModel m_lastRenderedModel;
  bool m_hasRenderedModel = false;
  ElfOverlay m_elf;
  bool m_speedInitialized = false;
  float m_speedInputKmh = 0;
  float m_displaySpeedKmh = 0;
  float m_speedFromKmh = 0;
  uint32_t m_speedTransitionAt = 0;
};
