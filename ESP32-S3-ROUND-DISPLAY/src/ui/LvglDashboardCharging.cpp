#include "ui/LvglDashboard.h"

#include <stdio.h>

#include "ui/DashboardUi.h"

using namespace dashui;

void LvglDashboard::buildCharging() {
  lv_obj_t* screen = lv_screen_active();
  m_chargeGroup = lv_obj_create(screen);
  lv_obj_remove_style_all(m_chargeGroup);
  lv_obj_set_size(m_chargeGroup, 466, 466);
  lv_obj_set_scrollable(m_chargeGroup, false);

  m_chargePowerLabel = makeLabel(m_chargeGroup, 0, 137, 466, 60,
                                 "--.- kW", &lv_font_montserrat_48, kText,
                                 LV_TEXT_ALIGN_CENTER);
  m_chargeCaption = makeLabel(m_chargeGroup, 0, 221, 466, 18,
                              "CHARGING", &lv_font_montserrat_10, kGreen,
                              LV_TEXT_ALIGN_CENTER);

  lv_obj_t* socTrack = lv_obj_create(m_chargeGroup);
  lv_obj_remove_style_all(socTrack);
  lv_obj_set_pos(socTrack, 93, 251);
  lv_obj_set_size(socTrack, 280, 6);
  lv_obj_set_style_radius(socTrack, 3, 0);
  lv_obj_set_style_bg_color(socTrack, kTrack, 0);
  lv_obj_set_style_bg_opa(socTrack, LV_OPA_COVER, 0);

  m_socBarFill = lv_obj_create(socTrack);
  lv_obj_remove_style_all(m_socBarFill);
  lv_obj_set_pos(m_socBarFill, 0, 0);
  lv_obj_set_size(m_socBarFill, 0, 6);
  lv_obj_set_style_radius(m_socBarFill, 3, 0);
  lv_obj_set_style_bg_color(m_socBarFill, kGreen, 0);
  lv_obj_set_style_bg_opa(m_socBarFill, LV_OPA_COVER, 0);

  m_chargeEta = makeLabel(m_chargeGroup, 92, 263, 282, 18,
                          "0% SOC                   TO 80% -- MIN",
                          &lv_font_montserrat_10, kDim, LV_TEXT_ALIGN_CENTER);
}

void LvglDashboard::renderChargingPage(const UiModel& model) {
  if (model.chargePowerValid) {
    char chargePower[20];
    snprintf(chargePower, sizeof(chargePower), "%.1f kW",
             model.chargePowerKw);
    updateLabel(m_chargePowerLabel, chargePower);
  } else {
    updateLabel(m_chargePowerLabel, "--.- kW");
  }
  char caption[32];
  if (model.socValid) {
    snprintf(caption, sizeof(caption), "CHARGING · %d%%",
             static_cast<int>(model.soc));
  } else {
    snprintf(caption, sizeof(caption), "CHARGING");
  }
  updateLabel(m_chargeCaption, caption);
  if (model.socValid) {
    const int32_t soc =
        model.soc < 0 ? 0 : (model.soc > 100 ? 100 : model.soc);
    lv_obj_set_size(m_socBarFill, soc * 280 / 100, 6);
  } else {
    lv_obj_set_size(m_socBarFill, 0, 6);
  }
  const float remainingKwh =
      model.socValid && model.soc < 80
          ? (80.0f - static_cast<float>(model.soc)) * 0.75f
          : 0.0f;
  const bool etaValid =
      model.socValid && model.chargePowerValid && model.chargePowerKw > 0;
  const int32_t etaMinutes = etaValid
                                 ? static_cast<int32_t>(
                                       remainingKwh / model.chargePowerKw * 60.0f)
                                 : 0;
  char cardEta[16];
  if (etaValid) {
    snprintf(cardEta, sizeof(cardEta), "%d min",
             static_cast<int>(etaMinutes));
  } else {
    snprintf(cardEta, sizeof(cardEta), "-- min");
  }
  updateLabel(m_chargeEtaValue, cardEta);
  char cardPower[20];
  if (model.chargePowerValid) {
    snprintf(cardPower, sizeof(cardPower), "AT %.1f kW",
             model.chargePowerKw);
  } else {
    snprintf(cardPower, sizeof(cardPower), "AT -- kW");
  }
  updateLabel(m_chargeEtaSub, cardPower);
  if (model.rangeValid) {
    char chargeRange[20];
    snprintf(chargeRange, sizeof(chargeRange), "%ld km",
             static_cast<long>(model.rangeKm));
    updateLabel(m_chargeRangeValue, chargeRange);
  } else {
    updateLabel(m_chargeRangeValue, "-- km");
  }
  char rangeSoc[20];
  if (model.socValid) {
    snprintf(rangeSoc, sizeof(rangeSoc), "AT %d%% SOC",
             static_cast<int>(model.soc));
  } else {
    snprintf(rangeSoc, sizeof(rangeSoc), "AT --%% SOC");
  }
  updateLabel(m_chargeRangeSub, rangeSoc);
  char eta[48];
  snprintf(eta, sizeof(eta), "%d%% SOC                 TO 80%% %ld MIN",
           static_cast<int>(model.socValid ? model.soc : 0),
           static_cast<long>(etaMinutes));
  updateLabel(m_chargeEta, eta);
}
