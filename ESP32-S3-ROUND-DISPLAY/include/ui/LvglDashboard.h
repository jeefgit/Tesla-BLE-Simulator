#pragma once

#include <lvgl.h>

#include "core/UiModel.h"
#include "ui/IDashboard.h"

class LvglDashboard : public IDashboard {
 public:
  void begin() override;
  void render(const UiModel& model) override;
  void updateSpeedLabel(float speedKmh, bool valid);
  // True while the boot overlay covers the dashboard. The app uses this to
  // skip redrawing widgets nobody can see, which keeps the boot animation
  // smooth and leaves the CPU/BLE stack alone during startup.
  bool bootAnimationRunning() const;

 private:
  void buildGauge();
  void buildTopBar();
  void buildDriving();
  void buildCharging();
  void buildCards();
  void buildLinkPage();
  void buildPairingPage();
  void buildBootAnimation();
  void startLinkAnimation();
  void stopLinkAnimation();
  void startPairAnimation();
  void stopPairAnimation();

  void setHidden(lv_obj_t* object, bool hidden);
  void updateLabel(lv_obj_t* label, const char* text);
  void updateGaugeArc(float speedKmh, bool valid);

  lv_obj_t* m_arc = nullptr;
  bool m_showSpeedArc = false;
  lv_obj_t* m_gaugeMinLabel = nullptr;
  lv_obj_t* m_gaugeMaxLabel = nullptr;
  lv_obj_t* m_linkLabel = nullptr;
  lv_obj_t* m_linkDot = nullptr;
  lv_obj_t* m_lockLabel = nullptr;
  lv_obj_t* m_speedGroup = nullptr;
  lv_obj_t* m_speedLabel = nullptr;
  lv_obj_t* m_gearHighlight = nullptr;
  lv_obj_t* m_gearLabels[4] = {};
  lv_obj_t* m_powerLabel = nullptr;
  lv_obj_t* m_powerFill = nullptr;
  lv_obj_t* m_chargeGroup = nullptr;
  lv_obj_t* m_chargePowerLabel = nullptr;
  lv_obj_t* m_chargeCaption = nullptr;
  lv_obj_t* m_socBarFill = nullptr;
  lv_obj_t* m_chargeEta = nullptr;
  lv_obj_t* m_cardsGroup = nullptr;
  lv_obj_t* m_chargeCardsGroup = nullptr;
  lv_obj_t* m_socLabel = nullptr;
  lv_obj_t* m_socBar = nullptr;
  lv_obj_t* m_rangeLabel = nullptr;
  lv_obj_t* m_chargeEtaValue = nullptr;
  lv_obj_t* m_chargeEtaSub = nullptr;
  lv_obj_t* m_chargeRangeValue = nullptr;
  lv_obj_t* m_chargeRangeSub = nullptr;
  lv_obj_t* m_temperatureGroup = nullptr;
  lv_obj_t* m_outsideTempLabel = nullptr;
  lv_obj_t* m_insideTempLabel = nullptr;
  lv_obj_t* m_linkPage = nullptr;
  lv_obj_t* m_linkTitle = nullptr;
  lv_obj_t* m_linkSubtitle = nullptr;
  lv_obj_t* m_linkMeta = nullptr;
  lv_obj_t* m_scanArcOuter = nullptr;
  lv_obj_t* m_scanArcInner = nullptr;
  lv_obj_t* m_scanPulse = nullptr;
  bool m_linkAnimationRunning = false;
  lv_obj_t* m_pairPage = nullptr;
  lv_obj_t* m_pairTitle = nullptr;
  lv_obj_t* m_pairCount = nullptr;
  lv_obj_t* m_pairStatus = nullptr;
  lv_obj_t* m_pairTry = nullptr;
  lv_obj_t* m_pairBarFill = nullptr;
  lv_obj_t* m_pairCardGlow = nullptr;
  lv_obj_t* m_pairPulse = nullptr;
  bool m_pairAnimationRunning = false;
  lv_obj_t* m_topGroup = nullptr;
  lv_obj_t* m_bootLayer = nullptr;
  lv_obj_t* m_bootRing = nullptr;
  lv_obj_t* m_bootEmblem = nullptr;
};
