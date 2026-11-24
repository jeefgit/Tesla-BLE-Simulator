#include "ui/LvglDashboard.h"

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "ui/DashboardUi.h"

using namespace dashui;

// The dashboard is split across several translation units, one per page:
//   LvglDashboardBoot.cpp     boot ring + logo overlay
//   LvglDashboardVin.cpp      VIN editor overlay
//   LvglDashboardDriving.cpp  gauge, top bar, speed/gear/power, battery cards
//   LvglDashboardCharging.cpp charging page
//   LvglDashboardLink.cpp     link/scan page + config page
//   LvglDashboardPairing.cpp  key pairing page
//   LvglDashboardDoor.cpp     door-open page
// This file owns the screen lifecycle, shared visibility helpers and the
// per-frame render() dispatch.

void LvglDashboard::begin() {
  lv_obj_t* screen = lv_screen_active();
  lv_obj_set_style_bg_color(screen, kScreen, 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(screen, 0, 0);
  lv_obj_set_scrollable(screen, false);

  buildGauge();
  buildTopBar();
  buildDriving();
  buildCharging();
  buildCards();
  buildLinkPage();
  buildPairingPage();
  buildDoorPage();
  buildBootAnimation();
#if CFG_VIN_INPUT_ENABLE
  buildVinEditor();
#endif
}

void LvglDashboard::setConfigMode(bool active) {
  if (m_configMode == active) return;
  m_configMode = active;
  if (!active) return;
  // 按需构建：配置页十几个控件常驻会占掉几 KB 系统堆（LVGL 走 malloc），
  // 而正常仪表模式下 BLE 连接任务需要 8KB 连续内存，差一点就创建失败。
  // 配置模式下 BLE 不启动，所以只在进入配置模式时才建页面。
  buildConfigPage();
  // 页面要盖在仪表之上，但不能盖住开机动画层，重新把开机层提到最前。
  lv_obj_move_foreground(m_bootLayer);
}

void LvglDashboard::setConfigInfo(const char* ssid, const char* ip) {
  if (m_configSsidLabel == nullptr || m_configIpLabel == nullptr) return;
  char line[40];
  snprintf(line, sizeof(line), "%s", ssid ? ssid : "--");
  updateLabel(m_configSsidLabel, line);
  snprintf(line, sizeof(line), "http://%s", ip ? ip : "--");
  updateLabel(m_configIpLabel, line);
}

bool LvglDashboard::linkPageVisible() const {
  return m_linkPage != nullptr &&
         !lv_obj_has_flag(m_linkPage, LV_OBJ_FLAG_HIDDEN);
}

void LvglDashboard::setHidden(lv_obj_t* object, bool hidden) {
  if (hidden) {
    lv_obj_set_hidden(object, true);
  } else {
    lv_obj_set_hidden(object, false);
  }
}

void LvglDashboard::updateLabel(lv_obj_t* label, const char* text) {
  if (strcmp(lv_label_get_text(label), text) != 0) {
    lv_label_set_text(label, text);
  }
}

void LvglDashboard::render(const UiModel& model) {
  const bool pairing =
      model.link == UiLinkState::Ready &&
      model.pairing != UiPairingPhase::Idle;
  const bool ready = model.link == UiLinkState::Ready;
  // 配置模式独占屏幕：不显示链接/扫描页，只显示配置信息页。
  const bool showLinkPage = !pairing && !ready && !m_configMode;
  uint8_t openDoorCount = 0;
  int8_t firstOpenDoor = -1;
  for (uint8_t i = 0; i < 6; ++i) {
    if (model.doorsValid && model.doors[i]) {
      ++openDoorCount;
      if (firstOpenDoor < 0) {
        firstOpenDoor = static_cast<int8_t>(i);
      }
    }
  }
  const bool showDoorPage = ready && !pairing && openDoorCount > 0;
  const bool showCharging = ready && model.charging && !pairing &&
                            !showDoorPage;

  setHidden(m_speedGroup,
            !ready || showCharging || pairing || showDoorPage);
  setHidden(m_chargeGroup, !showCharging);
  setHidden(m_cardsGroup, !ready || showCharging || pairing || showDoorPage);
  setHidden(m_chargeCardsGroup, !showCharging);
  setHidden(m_temperatureGroup, !ready || pairing || showDoorPage);
  setHidden(m_arc, showLinkPage || pairing || showDoorPage);
  setHidden(m_gaugeTicks,
            showLinkPage || pairing || showCharging || showDoorPage);
  setHidden(m_gaugeMinLabel, pairing || showDoorPage);
  setHidden(m_gaugeMaxLabel, pairing || showDoorPage);
  setHidden(m_topGroup, pairing);
  setHidden(m_linkPage, !showLinkPage);
  // 配置页按需构建：正常模式下不存在（m_configPage 为空），跳过即可。
  if (m_configPage != nullptr) {
    setHidden(m_configPage, !m_configMode);
  }
  setHidden(m_pairPage, !pairing);
  setHidden(m_doorPage, !showDoorPage);
  if (showDoorPage) {
    renderDoorPage(model, openDoorCount, firstOpenDoor);
  }
  const bool pairAnimationActive =
      pairing && model.pairing != UiPairingPhase::Success &&
      model.pairing != UiPairingPhase::Failed;
  if (pairAnimationActive) {
    startPairAnimation();
  } else {
    stopPairAnimation();
  }
  if (showLinkPage) {
    startLinkAnimation();
  } else {
    stopLinkAnimation();
  }

  renderGaugeState(model, ready, pairing, showCharging);
  renderTopBar(model, pairing);
  updateSpeedLabel(model.speedKmh, model.speedValid);
  renderDrivingPage(model);

  if (showCharging) {
    renderChargingPage(model);
  }
  if (showLinkPage) {
    renderLinkPage(model);
  }
  if (pairing) {
    renderPairingPage(model);
  }
}
