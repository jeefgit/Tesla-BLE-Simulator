#include "ui/LvglDashboard.h"

#include <stdio.h>

#include "config.h"
#include "ui/DashboardUi.h"

using namespace dashui;

void LvglDashboard::buildLinkPage() {
  lv_obj_t* screen = lv_screen_active();
  m_linkPage = lv_obj_create(screen);
  lv_obj_remove_style_all(m_linkPage);
  lv_obj_set_size(m_linkPage, 466, 466);
  lv_obj_set_scrollable(m_linkPage, false);

  m_scanArcOuter = makeScanArc(m_linkPage, 148, 4, 159, 92,
                               kScanCyan, 20);
  m_scanArcInner = makeScanArc(m_linkPage, 108, 3, 179, 112,
                               kAmber, 24);
  m_scanPulse = lv_obj_create(m_linkPage);
  lv_obj_remove_style_all(m_scanPulse);
  lv_obj_set_size(m_scanPulse, 8, 8);
  lv_obj_set_pos(m_scanPulse, 229, 162);
  lv_obj_set_style_radius(m_scanPulse, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(m_scanPulse, kScanCyan, 0);
  lv_obj_set_style_bg_opa(m_scanPulse, LV_OPA_COVER, 0);
  lv_obj_set_clickable(m_scanPulse, false);
  makeLabel(m_linkPage, 203, 177, 60, 18, "BLE",
            &lv_font_montserrat_10, kDim, LV_TEXT_ALIGN_CENTER);
  makeLabel(m_linkPage, 33, 74, 400, 18, "TESLA BLE",
            &lv_font_montserrat_10, kBootRingColor, LV_TEXT_ALIGN_CENTER);
  m_linkTitle = makeScaledLabel(m_linkPage, 233, 274, "SCANNING",
                                &lv_font_montserrat_48, kText, 300);
  m_linkSubtitle =
      makeLabel(m_linkPage, 31, 306, 404, 20,
                "KEEP VEHICLE AWAKE",
                &lv_font_montserrat_10, kDim, LV_TEXT_ALIGN_CENTER);
  m_linkMeta = makeLabel(m_linkPage, 40, 333, 386, 18, "ELAPSED 0s",
                         &lv_font_montserrat_10, kFaint,
                         LV_TEXT_ALIGN_CENTER);
  m_linkVin = makeLabel(m_linkPage, 40, 350, 386, 18, "VIN --",
                        &lv_font_montserrat_12, kFaint,
                        LV_TEXT_ALIGN_CENTER);
  m_linkVinHint = makeLabel(m_linkPage, 46, 369, 374, 16,
                            "TAP VIN TO CHANGE", &lv_font_montserrat_10,
                            kFaint, LV_TEXT_ALIGN_CENTER);
}

void LvglDashboard::buildConfigPage() {
  lv_obj_t* screen = lv_screen_active();
  m_configPage = lv_obj_create(screen);
  lv_obj_remove_style_all(m_configPage);
  lv_obj_set_size(m_configPage, 466, 466);
  lv_obj_set_scrollable(m_configPage, false);

  makeLabel(m_configPage, 0, 64, 466, 18, "CONFIG MODE",
            &lv_font_montserrat_10, kBootRingColor, LV_TEXT_ALIGN_CENTER);
  makeScaledLabel(m_configPage, 233, 138, "CONFIG",
                  &lv_font_montserrat_48, kText, 300);
  makeLabel(m_configPage, 0, 186, 466, 18, "JOIN THE HOTSPOT ON YOUR PHONE",
            &lv_font_montserrat_10, kDim, LV_TEXT_ALIGN_CENTER);

  // 信息面板：SSID / 密码 / 网页地址三行。
  lv_obj_t* panel = makePanel(m_configPage, 78, 214, 310, 112, 16);
  makeLabel(panel, 20, 14, 56, 16, "SSID", &lv_font_montserrat_10, kFaint,
            LV_TEXT_ALIGN_LEFT);
  m_configSsidLabel = makeLabel(panel, 80, 8, 210, 24, "--",
                                &lv_font_montserrat_18, kText,
                                LV_TEXT_ALIGN_LEFT);
  makeLabel(panel, 20, 48, 56, 16, "PASS", &lv_font_montserrat_10, kFaint,
            LV_TEXT_ALIGN_LEFT);
  makeLabel(panel, 80, 42, 210, 24, CFG_WEB_AP_PASSWORD,
            &lv_font_montserrat_18, kText, LV_TEXT_ALIGN_LEFT);
  makeLabel(panel, 20, 82, 56, 16, "URL", &lv_font_montserrat_10, kFaint,
            LV_TEXT_ALIGN_LEFT);
  m_configIpLabel = makeLabel(panel, 80, 76, 210, 24, "http://--",
                              &lv_font_montserrat_18, kScanCyan,
                              LV_TEXT_ALIGN_LEFT);

  makeLabel(m_configPage, 0, 350, 466, 18, "OPEN THE PAGE TO SET VIN / OTA",
            &lv_font_montserrat_10, kDim, LV_TEXT_ALIGN_CENTER);
  makeLabel(m_configPage, 0, 424, 466, 18, "LONG PRESS BOOT TO EXIT",
            &lv_font_montserrat_10, kFaint, LV_TEXT_ALIGN_CENTER);

  lv_obj_set_hidden(m_configPage, true);
}

void LvglDashboard::startLinkAnimation() {
  if (m_linkAnimationRunning) return;
  m_linkAnimationRunning = true;
  startScanArcAnimation(m_scanArcOuter, 0, 360, 2600);
  startScanArcAnimation(m_scanArcInner, 360, 0, 1900);

  lv_anim_t pulse;
  lv_anim_init(&pulse);
  lv_anim_set_var(&pulse, m_scanPulse);
  lv_anim_set_values(&pulse, 70, LV_OPA_COVER);
  lv_anim_set_duration(&pulse, 650);
  lv_anim_set_playback_time(&pulse, 650);
  lv_anim_set_exec_cb(&pulse, setScanPulseOpacity);
  lv_anim_set_repeat_count(&pulse, LV_ANIM_REPEAT_INFINITE);
  lv_anim_start(&pulse);
}

void LvglDashboard::stopLinkAnimation() {
  if (!m_linkAnimationRunning) return;
  lv_anim_delete(m_scanArcOuter, rotateScanArc);
  lv_anim_delete(m_scanArcInner, rotateScanArc);
  lv_anim_delete(m_scanPulse, setScanPulseOpacity);
  m_linkAnimationRunning = false;
  lv_arc_set_rotation(m_scanArcOuter, 0);
  lv_arc_set_rotation(m_scanArcInner, 0);
  lv_obj_set_style_opa(m_scanPulse, LV_OPA_COVER, 0);
}

void LvglDashboard::renderLinkPage(const UiModel& model) {
  const char* title = "SCANNING";
  const char* subtitle = "KEEP VEHICLE AWAKE";
  switch (model.link) {
    case UiLinkState::Idle:
      title = "BLE IDLE";
      subtitle = "STACK NOT STARTED";
      break;
    case UiLinkState::Connecting:
      title = "CONNECTING";
      subtitle = "OPENING BLE LINK TO VEHICLE";
      break;
    case UiLinkState::Syncing:
      title = "SYNCING";
      subtitle = "WAITING FOR VEHICLE DATA";
      break;
    case UiLinkState::Reconnecting:
      title = "RECONNECTING";
      subtitle = "LINK LOST - RETRYING";
      break;
    case UiLinkState::Error:
      title = "LINK ERROR";
      subtitle = "CHECK VEHICLE AND RETRY";
      break;
    case UiLinkState::Ready:
    case UiLinkState::Scanning:
      break;
  }
  updateLabel(m_linkTitle, title);
  updateLabel(m_linkSubtitle, subtitle);
  const lv_color_t statusColor =
      model.link == UiLinkState::Error ? kRed : kBootRingColor;
  lv_obj_set_style_text_color(m_linkTitle, statusColor, 0);
  char meta[24];
  snprintf(meta, sizeof(meta), "ELAPSED %lus",
           static_cast<unsigned long>(model.elapsedSeconds));
  updateLabel(m_linkMeta, meta);

  char vinText[32];
  if (model.vin[0] == '\0') {
    snprintf(vinText, sizeof(vinText), "VIN NOT SET");
    lv_obj_set_style_text_color(m_linkVin, kAmber, 0);
  } else {
    snprintf(vinText, sizeof(vinText), "VIN %s", model.vin);
    lv_obj_set_style_text_color(m_linkVin, kFaint, 0);
  }
  updateLabel(m_linkVin, vinText);
}
