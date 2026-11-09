#pragma once

#include <lvgl.h>

#include "core/UiModel.h"
#include "ui/IDashboard.h"

class LvglDashboard : public IDashboard {
 public:
  void begin() override;
  void render(const UiModel& model) override;

 private:
  void buildGauge();
  void buildTopBar();
  void buildDriving();
  void buildCharging();
  void buildCards();
  void buildLinkPage();
  void buildPairingPage();

  void setHidden(lv_obj_t* object, bool hidden);
  void updateLabel(lv_obj_t* label, const char* text);

  lv_obj_t* m_arc = nullptr;
  lv_obj_t* m_gaugeMinLabel = nullptr;
  lv_obj_t* m_gaugeMaxLabel = nullptr;
  lv_obj_t* m_linkLabel = nullptr;
  lv_obj_t* m_linkDot = nullptr;
  lv_obj_t* m_lockLabel = nullptr;
  lv_obj_t* m_speedGroup = nullptr;
  lv_obj_t* m_speedLabel = nullptr;
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
  lv_obj_t* m_temperatureLabel = nullptr;
  lv_obj_t* m_linkPage = nullptr;
  lv_obj_t* m_linkTitle = nullptr;
  lv_obj_t* m_linkSubtitle = nullptr;
  lv_obj_t* m_linkMeta = nullptr;
  lv_obj_t* m_pairPage = nullptr;
  lv_obj_t* m_pairTitle = nullptr;
  lv_obj_t* m_pairCount = nullptr;
  lv_obj_t* m_pairStatus = nullptr;
  lv_obj_t* m_pairTry = nullptr;
  lv_obj_t* m_pairBarFill = nullptr;
  lv_obj_t* m_topGroup = nullptr;
};
