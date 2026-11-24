#pragma once

#include <lvgl.h>

#include <functional>
#include <stdint.h>

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
  // True while the link/scan page is on screen. The app uses this to decide
  // whether a tap may fall on the VIN row.
  bool linkPageVisible() const;

  // 配置模式页面：显示热点 SSID / 密码 / 网页地址，替代 SCANNING 链接页。
  // setConfigMode 必须在 render() 之前调用；setConfigInfo 更新 SSID/IP 文本。
  void setConfigMode(bool active);
  void setConfigInfo(const char* ssid, const char* ip);

  // VIN editor overlay. Pure UI: it edits a local buffer and reports submit /
  // cancel through callbacks; NVS writes and restart stay in the app.
  void vinEditorShow(const char* initialVin);
  void vinEditorHide();
  bool vinEditorVisible() const { return m_vinEditorVisible; }

  // Returns true when the editor consumed the touch (hit or not).
  bool vinEditorHandleTap(int16_t x, int16_t y);

  void vinEditorSetError(const char* text);
  void vinEditorShowSaved();

  void vinEditorSetSubmitHandler(std::function<void(const char*)> handler);
  void vinEditorSetCancelHandler(std::function<void()> handler);

 private:
  void buildGauge();
  void buildTopBar();
  void buildDriving();
  void buildCharging();
  void buildCards();
  void buildLinkPage();
  void buildConfigPage();
  void buildPairingPage();
  void buildDoorPage();
  void buildBootAnimation();
  void buildVinEditor();
  void vinEditorSetLayout(bool numeric);
  void vinEditorRefresh();
  int vinHitKey(int16_t x, int16_t y) const;
  void vinEditorSubmitOrError();
  void startLinkAnimation();
  void stopLinkAnimation();
  void startPairAnimation();
  void stopPairAnimation();

  void setHidden(lv_obj_t* object, bool hidden);
  void updateLabel(lv_obj_t* label, const char* text);
  void updateGaugeArc(float speedKmh, bool valid);

  // Per-page render helpers. Each one is implemented in the translation unit
  // that also builds the page, so widgets and their update code stay together.
  void renderTopBar(const UiModel& model, bool pairing);
  void renderGaugeState(const UiModel& model, bool ready, bool pairing,
                        bool showCharging);
  void renderDrivingPage(const UiModel& model);
  void renderChargingPage(const UiModel& model);
  void renderLinkPage(const UiModel& model);
  void renderPairingPage(const UiModel& model);
  void renderDoorPage(const UiModel& model, uint8_t openDoorCount,
                      int8_t firstOpenDoor);

  lv_obj_t* m_arc = nullptr;
  bool m_showSpeedArc = false;
  lv_obj_t* m_gaugeTicks = nullptr;
  lv_obj_t* m_gaugeMinLabel = nullptr;
  lv_obj_t* m_gaugeMaxLabel = nullptr;
  lv_obj_t* m_linkLabel = nullptr;
  lv_obj_t* m_linkDot = nullptr;
  lv_obj_t* m_lockLabel = nullptr;
  lv_obj_t* m_speedGroup = nullptr;
  lv_obj_t* m_speedLabel = nullptr;
  lv_obj_t* m_speedLatencyLabel = nullptr;
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
  lv_obj_t* m_linkVin = nullptr;
  lv_obj_t* m_linkVinHint = nullptr;
  bool m_configMode = false;
  lv_obj_t* m_configPage = nullptr;
  lv_obj_t* m_configSsidLabel = nullptr;
  lv_obj_t* m_configIpLabel = nullptr;
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
  lv_obj_t* m_pairCard = nullptr;
  lv_obj_t* m_pairChip = nullptr;
  lv_obj_t* m_pairStripe1 = nullptr;
  lv_obj_t* m_pairStripe2 = nullptr;
  lv_obj_t* m_pairSuccess = nullptr;   // 成功态绿色对勾徽章（默认隐藏）
  lv_obj_t* m_pairNfcLabel = nullptr;  // "NFC READY"
  lv_obj_t* m_pairHint = nullptr;      // "WINDOW CLOSES AUTOMATICALLY"
  bool m_pairAnimationRunning = false;
  lv_obj_t* m_doorPage = nullptr;
  lv_obj_t* m_doorTitle = nullptr;
  lv_obj_t* m_doorCount = nullptr;
  lv_obj_t* m_doorIndicators[6] = {};
  lv_obj_t* m_doorIndicatorCenters[6] = {};
  lv_obj_t* m_doorCallouts[6] = {};
  lv_obj_t* m_doorPanels[4] = {};
  uint8_t m_doorDisplayMask = 0xFF;
  lv_obj_t* m_topGroup = nullptr;
  lv_obj_t* m_bootLayer = nullptr;
  lv_obj_t* m_bootRing = nullptr;
  lv_obj_t* m_bootEmblem = nullptr;

  // VIN editor: created once in begin() above the boot layer, shown on demand.
  lv_obj_t* m_vinEditor = nullptr;
  lv_obj_t* m_vinTitle = nullptr;
  lv_obj_t* m_vinText = nullptr;
  lv_obj_t* m_vinCounter = nullptr;
  lv_obj_t* m_vinStatus = nullptr;
  lv_obj_t* m_vinKeys[30] = {};
  lv_obj_t* m_vinKeyLabels[30] = {};
  bool m_vinKeyVisible[30] = {};
  bool m_vinEditorVisible = false;
  bool m_vinNumericLayout = false;
  char m_vinEdit[18] = {};
  uint8_t m_vinEditLen = 0;
  char m_vinStatusText[32] = {};
  std::function<void(const char*)> m_vinSubmitHandler;
  std::function<void()> m_vinCancelHandler;
};
