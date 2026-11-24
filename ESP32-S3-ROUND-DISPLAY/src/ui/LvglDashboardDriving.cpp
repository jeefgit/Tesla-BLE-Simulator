#include "ui/LvglDashboard.h"

#include <math.h>
#include <stdio.h>

#include "ui/DashboardUi.h"

using namespace dashui;

namespace {

constexpr int32_t kArcMaxValue = 1000;
constexpr float kGaugeMaxSpeedKmh = 150.0f;
// Motor power bar: the track is 268px wide with the zero mark at x=134, so
// each direction has 134px of travel. Asymmetric range: -100kW regen (green,
// left) and +150kW output (red, right), each scaled to fill its half.
constexpr float kPowerRegenMaxKw = 100.0f;
constexpr float kPowerOutMaxKw = 150.0f;
constexpr float kPowerHalfWidthPx = 134.0f;

}  // namespace

void LvglDashboard::buildGauge() {
  lv_obj_t* screen = lv_screen_active();
  m_arc = lv_arc_create(screen);
  lv_obj_set_pos(m_arc, 63, 63);
  lv_obj_set_size(m_arc, 340, 340);
  lv_arc_set_rotation(m_arc, 204);
  lv_arc_set_bg_angles(m_arc, 0, 132);
  lv_arc_set_range(m_arc, 0, kArcMaxValue);
  lv_arc_set_value(m_arc, 0);
  lv_arc_set_mode(m_arc, LV_ARC_MODE_NORMAL);
  lv_obj_set_style_bg_opa(m_arc, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(m_arc, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(m_arc, 0, LV_PART_MAIN);
  lv_obj_set_style_arc_width(m_arc, 6, LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(m_arc, true, LV_PART_MAIN);
  lv_obj_set_style_arc_color(m_arc, kTrack, LV_PART_MAIN);
  lv_obj_set_style_arc_width(m_arc, 6, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(m_arc, true, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(m_arc, kGaugeColor, LV_PART_INDICATOR);
  lv_obj_set_style_pad_all(m_arc, 0, LV_PART_KNOB);
  lv_obj_set_style_bg_opa(m_arc, LV_OPA_TRANSP, LV_PART_KNOB);
  lv_obj_set_style_border_width(m_arc, 0, LV_PART_KNOB);
  lv_obj_set_clickable(m_arc, false);

  // Speed ticks at 30 / 60 / 90 / 120 km/h. LVGL angle 0 is 3 o'clock and
  // grows clockwise; the arc starts at 204 deg and spans 132 deg over
  // 0..kGaugeMaxSpeedKmh, so a tick sits at
  //   204 + 132 * speed / kGaugeMaxSpeedKmh
  // degrees. The short radial line straddles the R167 arc midline. All tick
  // numbers sit straight outside the ring at R190, radially centred on their
  // own tick (same style as 30 / 120). At 60/90 they land in the horizontal
  // gaps of the transparent top status bar, so they do not overlap its text.
  constexpr float kArcStartDeg = 204.0f;
  constexpr float kArcSpanDeg = 132.0f;
  constexpr float kGaugeCenter = 233.0f;
  constexpr float kTickInnerRadius = 162.0f;
  constexpr float kTickOuterRadius = 172.0f;
  constexpr float kTickLabelRadius = 190.0f;
  constexpr float kDegToRad = 3.14159265f / 180.0f;
  constexpr int32_t kTickSpeeds[] = {30, 60, 90, 120};
  constexpr int32_t kTickLabelWidth = 24;
  constexpr int32_t kTickLabelHeight = 16;

  m_gaugeTicks = lv_obj_create(screen);
  lv_obj_remove_style_all(m_gaugeTicks);
  lv_obj_set_pos(m_gaugeTicks, 0, 0);
  lv_obj_set_size(m_gaugeTicks, kScreenSize, kScreenSize);
  lv_obj_set_scrollable(m_gaugeTicks, false);
  lv_obj_set_clickable(m_gaugeTicks, false);

  // lv_line keeps only the pointer to its points, so the array must outlive
  // buildGauge().
  static lv_point_precise_t tickPoints[4][2];
  for (int32_t i = 0; i < 4; ++i) {
    const float angle =
        (kArcStartDeg + kArcSpanDeg * static_cast<float>(kTickSpeeds[i]) /
                             kGaugeMaxSpeedKmh) *
        kDegToRad;
    const float c = cosf(angle);
    const float s = sinf(angle);

    tickPoints[i][0].x =
        static_cast<lv_value_precise_t>(lroundf(kGaugeCenter +
                                                c * kTickInnerRadius));
    tickPoints[i][0].y =
        static_cast<lv_value_precise_t>(lroundf(kGaugeCenter +
                                                s * kTickInnerRadius));
    tickPoints[i][1].x =
        static_cast<lv_value_precise_t>(lroundf(kGaugeCenter +
                                                c * kTickOuterRadius));
    tickPoints[i][1].y =
        static_cast<lv_value_precise_t>(lroundf(kGaugeCenter +
                                                s * kTickOuterRadius));

    lv_obj_t* tick = lv_line_create(m_gaugeTicks);
    lv_line_set_points(tick, tickPoints[i], 2);
    lv_obj_set_style_line_width(tick, 2, 0);
    lv_obj_set_style_line_color(tick, kFaint, 0);
    lv_obj_set_style_line_rounded(tick, true, 0);
    lv_obj_set_clickable(tick, false);

    char tickText[8];
    snprintf(tickText, sizeof(tickText), "%ld",
             static_cast<long>(kTickSpeeds[i]));
    const int32_t labelX =
        static_cast<int32_t>(
            lroundf(kGaugeCenter + c * kTickLabelRadius)) -
        kTickLabelWidth / 2;
    const int32_t labelY =
        static_cast<int32_t>(
            lroundf(kGaugeCenter + s * kTickLabelRadius)) -
        kTickLabelHeight / 2;
    makeLabel(m_gaugeTicks, labelX, labelY, kTickLabelWidth,
              kTickLabelHeight, tickText, &lv_font_montserrat_10, kFaint,
              LV_TEXT_ALIGN_CENTER);
  }

  m_gaugeMinLabel = makeLabel(screen, 47, 150, 30, 18, "0",
                              &lv_font_montserrat_10, kFaint,
                              LV_TEXT_ALIGN_CENTER);
  m_gaugeMaxLabel = makeLabel(screen, 389, 150, 30, 18, "150",
                              &lv_font_montserrat_10, kFaint,
                              LV_TEXT_ALIGN_CENTER);
}

void LvglDashboard::buildTopBar() {
  lv_obj_t* screen = lv_screen_active();
  m_topGroup = lv_obj_create(screen);
  lv_obj_remove_style_all(m_topGroup);
  lv_obj_set_size(m_topGroup, 240, 20);
  lv_obj_set_pos(m_topGroup, 113, 43);
  lv_obj_set_scrollable(m_topGroup, false);

  makeLabel(m_topGroup, 0, 0, 60, 18, "TESLA", &lv_font_montserrat_14,
            lv_color_hex(0xC9D3DE), LV_TEXT_ALIGN_LEFT);
  m_linkDot = lv_obj_create(m_topGroup);
  lv_obj_remove_style_all(m_linkDot);
  lv_obj_set_size(m_linkDot, 5, 5);
  lv_obj_set_pos(m_linkDot, 100, 6);
  lv_obj_set_style_radius(m_linkDot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(m_linkDot, kGreen, 0);
  lv_obj_set_style_bg_opa(m_linkDot, LV_OPA_COVER, 0);
  m_linkLabel = makeLabel(m_topGroup, 109, 0, 60, 18, "LINK",
                          &lv_font_montserrat_14, kText, LV_TEXT_ALIGN_LEFT);
  m_lockLabel = makeLabel(m_topGroup, 176, 0, 64, 18, "LOCK",
                          &lv_font_montserrat_14, kFaint, LV_TEXT_ALIGN_RIGHT);
}

void LvglDashboard::buildDriving() {
  lv_obj_t* screen = lv_screen_active();
  m_speedGroup = lv_obj_create(screen);
  lv_obj_remove_style_all(m_speedGroup);
  lv_obj_set_size(m_speedGroup, 466, 466);
  lv_obj_set_scrollable(m_speedGroup, false);

  m_speedLabel = makeScaledLabel(m_speedGroup, 233, 155, "--",
                                 &lv_font_montserrat_48, kText, 600);
  makeLabel(m_speedGroup, 0, 218, 466, 18, "KM/H",
            &lv_font_montserrat_14, kGaugeColor, LV_TEXT_ALIGN_CENTER);

  // 车速获取延迟（请求发出 → 解析完成）。放在状态栏上方的空闲区，
  // 只在驾驶页随 m_speedGroup 一起显示。
  m_speedLatencyLabel = makeLabel(m_speedGroup, 0, 24, 466, 14, "LATENCY -- MS",
                                  &lv_font_montserrat_10, kFaint,
                                  LV_TEXT_ALIGN_CENTER);

  const char* gears[] = {"P", "R", "N", "D"};
  const int32_t gearX[] = {155, 197, 239, 281};
  lv_obj_t* gearTrack = lv_obj_create(m_speedGroup);
  lv_obj_remove_style_all(gearTrack);
  lv_obj_set_pos(gearTrack, 149, 248);
  lv_obj_set_size(gearTrack, 168, 40);
  lv_obj_set_style_radius(gearTrack, 12, 0);
  lv_obj_set_style_bg_color(gearTrack, lv_color_hex(0x0A1117), 0);
  lv_obj_set_style_bg_opa(gearTrack, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(gearTrack, 1, 0);
  lv_obj_set_style_border_color(gearTrack, lv_color_hex(0x202D37), 0);
  m_gearHighlight = lv_obj_create(m_speedGroup);
  lv_obj_remove_style_all(m_gearHighlight);
  lv_obj_set_size(m_gearHighlight, 36, 34);
  lv_obj_set_style_radius(m_gearHighlight, 9, 0);
  lv_obj_set_style_bg_color(m_gearHighlight, kGaugeColor, 0);
  lv_obj_set_style_bg_opa(m_gearHighlight, LV_OPA_COVER, 0);
  lv_obj_set_hidden(m_gearHighlight, true);

  for (int32_t i = 0; i < 4; ++i) {
    m_gearLabels[i] = makeLabel(m_speedGroup, gearX[i], 256, 30, 24,
                                gears[i], &lv_font_montserrat_20, kFaint,
                                LV_TEXT_ALIGN_CENTER);
  }

  lv_obj_t* powerPanel = makePanel(m_speedGroup, 83, 298, 300, 52, 12);
  lv_obj_set_style_border_color(powerPanel, lv_color_hex(0x283642), 0);
  makeLabel(powerPanel, 14, 8, 92, 16, "MOTOR POWER",
            &lv_font_montserrat_10, kFaint, LV_TEXT_ALIGN_LEFT);
  m_powerLabel = makeLabel(powerPanel, 112, 3, 172, 25, "--.- kW",
                           &lv_font_montserrat_20, kText, LV_TEXT_ALIGN_RIGHT);

  lv_obj_t* powerTrack = lv_obj_create(m_speedGroup);
  lv_obj_remove_style_all(powerTrack);
  lv_obj_set_pos(powerTrack, 98, 335);
  lv_obj_set_size(powerTrack, 268, 5);
  lv_obj_set_style_radius(powerTrack, 3, 0);
  lv_obj_set_style_bg_color(powerTrack, lv_color_hex(0x17232C), 0);
  lv_obj_set_style_bg_opa(powerTrack, LV_OPA_COVER, 0);

  lv_obj_t* zero = lv_obj_create(powerTrack);
  lv_obj_remove_style_all(zero);
  lv_obj_set_pos(zero, 133, -2);
  lv_obj_set_size(zero, 2, 9);
  lv_obj_set_style_bg_color(zero, lv_color_hex(0x637987), 0);
  lv_obj_set_style_bg_opa(zero, LV_OPA_COVER, 0);

  m_powerFill = lv_obj_create(powerTrack);
  lv_obj_remove_style_all(m_powerFill);
  lv_obj_set_pos(m_powerFill, 134, 0);
  lv_obj_set_size(m_powerFill, 0, 5);
  lv_obj_set_style_radius(m_powerFill, 3, 0);
  lv_obj_set_style_bg_color(m_powerFill, kRed, 0);
  lv_obj_set_style_bg_opa(m_powerFill, LV_OPA_COVER, 0);
}

void LvglDashboard::buildCards() {
  lv_obj_t* screen = lv_screen_active();
  m_cardsGroup = lv_obj_create(screen);
  lv_obj_remove_style_all(m_cardsGroup);
  lv_obj_set_size(m_cardsGroup, 466, 466);
  lv_obj_set_scrollable(m_cardsGroup, false);

  lv_obj_t* vehiclePanel = makePanel(m_cardsGroup, 83, 359, 300, 52, 12);
  lv_obj_set_style_border_color(vehiclePanel, lv_color_hex(0x283642), 0);
  makeLabel(vehiclePanel, 16, 5, 120, 12, "BATTERY",
            &lv_font_montserrat_10, kFaint, LV_TEXT_ALIGN_LEFT);
  m_socLabel = makeLabel(vehiclePanel, 16, 16, 120, 27, "--%",
                         &lv_font_montserrat_24, kText, LV_TEXT_ALIGN_LEFT);

  lv_obj_t* meterTrack = lv_obj_create(vehiclePanel);
  lv_obj_remove_style_all(meterTrack);
  lv_obj_set_pos(meterTrack, 16, 45);
  lv_obj_set_size(meterTrack, 120, 3);
  lv_obj_set_style_radius(meterTrack, 2, 0);
  lv_obj_set_style_bg_color(meterTrack, kTrack, 0);
  lv_obj_set_style_bg_opa(meterTrack, LV_OPA_COVER, 0);
  m_socBar = lv_obj_create(meterTrack);
  lv_obj_remove_style_all(m_socBar);
  lv_obj_set_pos(m_socBar, 0, 0);
  lv_obj_set_size(m_socBar, 0, 3);
  lv_obj_set_style_radius(m_socBar, 2, 0);
  lv_obj_set_style_bg_color(m_socBar, kText, 0);
  lv_obj_set_style_bg_opa(m_socBar, LV_OPA_COVER, 0);

  lv_obj_t* vehicleDivider = lv_obj_create(vehiclePanel);
  lv_obj_remove_style_all(vehicleDivider);
  lv_obj_set_pos(vehicleDivider, 150, 9);
  lv_obj_set_size(vehicleDivider, 1, 34);
  lv_obj_set_style_bg_color(vehicleDivider, lv_color_hex(0x25313A), 0);
  lv_obj_set_style_bg_opa(vehicleDivider, LV_OPA_COVER, 0);
  makeLabel(vehiclePanel, 166, 5, 118, 12, "RANGE",
            &lv_font_montserrat_10, kFaint, LV_TEXT_ALIGN_LEFT);
  m_rangeLabel = makeLabel(vehiclePanel, 166, 16, 118, 27, "-- km",
                           &lv_font_montserrat_24, kAmber, LV_TEXT_ALIGN_LEFT);

  m_chargeCardsGroup = lv_obj_create(screen);
  lv_obj_remove_style_all(m_chargeCardsGroup);
  lv_obj_set_size(m_chargeCardsGroup, 466, 466);
  lv_obj_set_scrollable(m_chargeCardsGroup, false);
  lv_obj_t* etaCard = makePanel(m_chargeCardsGroup, 95, 322, 132, 56);
  makeLabel(etaCard, 12, 8, 108, 12, "TO 80%",
            &lv_font_montserrat_10, kFaint, LV_TEXT_ALIGN_LEFT);
  m_chargeEtaValue = makeLabel(etaCard, 12, 19, 108, 27, "-- min",
                               &lv_font_montserrat_24, kText,
                               LV_TEXT_ALIGN_LEFT);
  m_chargeEtaSub = makeLabel(etaCard, 12, 43, 108, 11, "AT -- kW",
                             &lv_font_montserrat_10, kDim,
                             LV_TEXT_ALIGN_LEFT);

  lv_obj_t* chargeRange =
      makePanel(m_chargeCardsGroup, 239, 322, 132, 56);
  makeLabel(chargeRange, 12, 8, 108, 12, "RANGE",
            &lv_font_montserrat_10, kFaint, LV_TEXT_ALIGN_LEFT);
  m_chargeRangeValue = makeLabel(chargeRange, 12, 19, 108, 27, "-- km",
                                 &lv_font_montserrat_24, kText,
                                 LV_TEXT_ALIGN_LEFT);
  m_chargeRangeSub = makeLabel(chargeRange, 12, 43, 108, 11, "AT --% SOC",
                               &lv_font_montserrat_10, kDim,
                               LV_TEXT_ALIGN_LEFT);

  m_temperatureGroup = lv_obj_create(screen);
  lv_obj_remove_style_all(m_temperatureGroup);
  lv_obj_set_size(m_temperatureGroup, 466, 466);
  lv_obj_set_scrollable(m_temperatureGroup, false);

  lv_obj_t* temperaturePanel =
      makePanel(m_temperatureGroup, 113, 418, 240, 32, 10);
  makeLabel(temperaturePanel, 8, 9, 46, 14, "OUTSIDE",
            &lv_font_montserrat_10, kFaint, LV_TEXT_ALIGN_LEFT);
  m_outsideTempLabel = makeLabel(temperaturePanel, 54, 3, 62, 26, "--°C",
                                 &lv_font_montserrat_20, kText,
                                 LV_TEXT_ALIGN_LEFT);
  lv_obj_t* temperatureDivider = lv_obj_create(temperaturePanel);
  lv_obj_remove_style_all(temperatureDivider);
  lv_obj_set_pos(temperatureDivider, 120, 8);
  lv_obj_set_size(temperatureDivider, 1, 16);
  lv_obj_set_style_bg_color(temperatureDivider, lv_color_hex(0x25313A), 0);
  lv_obj_set_style_bg_opa(temperatureDivider, LV_OPA_COVER, 0);
  makeLabel(temperaturePanel, 132, 9, 38, 14, "CABIN",
            &lv_font_montserrat_10, kFaint, LV_TEXT_ALIGN_LEFT);
  m_insideTempLabel = makeLabel(temperaturePanel, 172, 3, 62, 26, "--°C",
                                &lv_font_montserrat_20, kText,
                                LV_TEXT_ALIGN_LEFT);
}

void LvglDashboard::updateSpeedLabel(float speedKmh, bool valid) {
  if (!valid) {
    updateLabel(m_speedLabel, "--");
  } else {
    char speed[12];
    snprintf(speed, sizeof(speed), "%.0f", speedKmh);
    updateLabel(m_speedLabel, speed);
  }

  // The arc lives outside render(): DashboardApp calls updateSpeedLabel every
  // loop with the smoothed speed, so drive the gauge from here to keep it in
  // sync with the numeric readout.
  if (m_showSpeedArc) {
    updateGaugeArc(speedKmh, valid);
  }
}

void LvglDashboard::updateGaugeArc(float speedKmh, bool valid) {
  float speed = 0.0f;
  if (valid) {
    speed = fminf(fmaxf(speedKmh, 0.0f), kGaugeMaxSpeedKmh);
  }
  const int32_t roundedSpeed = static_cast<int32_t>(lroundf(speed));
  const int32_t value = roundedSpeed * kArcMaxValue /
                        static_cast<int32_t>(kGaugeMaxSpeedKmh);
  lv_arc_set_value(m_arc, value);

  constexpr uint32_t speedColors[] = {0x43D9F5, 0x21C15E, 0xFFB340, 0xE82127};
  constexpr float colorSegments = 3.0f;
  const float colorPosition = speed / kGaugeMaxSpeedKmh * colorSegments;
  const uint32_t segment =
      static_cast<uint32_t>(fminf(colorPosition, colorSegments - 1.0f));
  const float blend = colorPosition - segment;
  const uint32_t from = speedColors[segment];
  const uint32_t to = speedColors[segment + 1];
  const uint8_t red = static_cast<uint8_t>(lroundf(
      ((from >> 16) & 0xFF) * (1.0f - blend) +
      ((to >> 16) & 0xFF) * blend));
  const uint8_t green = static_cast<uint8_t>(lroundf(
      ((from >> 8) & 0xFF) * (1.0f - blend) +
      ((to >> 8) & 0xFF) * blend));
  const uint8_t blue = static_cast<uint8_t>(lroundf(
      (from & 0xFF) * (1.0f - blend) + (to & 0xFF) * blend));
  lv_obj_set_style_arc_color(m_arc, lv_color_make(red, green, blue),
                             LV_PART_INDICATOR);
}

void LvglDashboard::renderTopBar(const UiModel& model, bool pairing) {
  const lv_color_t currentLinkColor = linkColor(model.link);
  lv_obj_set_style_bg_color(m_linkDot, currentLinkColor, 0);
  lv_obj_set_style_text_color(m_linkLabel, currentLinkColor, 0);
  updateLabel(m_linkLabel, pairing ? "PAIRING" : linkText(model.link));
  lv_obj_set_style_text_color(m_lockLabel,
                              model.locked ? kFaint : kAmber, 0);
  updateLabel(m_lockLabel,
              pairing ? "KEY" : (model.locked ? "LOCK" : "UNLOCK"));
}

void LvglDashboard::renderGaugeState(const UiModel& model, bool ready,
                                     bool pairing, bool showCharging) {
  updateLabel(m_gaugeMinLabel, "0");
  updateLabel(m_gaugeMaxLabel, showCharging ? "100" : "150");

  m_showSpeedArc = ready && !pairing && !showCharging;
  if (pairing) {
    lv_obj_set_style_arc_color(m_arc, kAmber, LV_PART_INDICATOR);
    lv_arc_set_value(m_arc, kArcMaxValue);
  } else if (showCharging) {
    lv_obj_set_style_arc_color(m_arc, kGreen, LV_PART_INDICATOR);
    lv_arc_set_value(m_arc, model.socValid ? model.soc * 10 : 0);
  } else if (m_showSpeedArc) {
    // The value is applied by updateSpeedLabel() at the end of render(), which
    // uses the same smoothed speed as the numeric readout.
  } else {
    lv_arc_set_value(m_arc, 0);
  }
  lv_obj_set_style_arc_color(m_arc,
                             ready ? kTrack : lv_color_hex(0x141C25),
                             LV_PART_MAIN);
}

void LvglDashboard::renderDrivingPage(const UiModel& model) {
  // 车速获取延迟：超过 500ms（Drive 轮询周期 300ms + BLE 抖动余量）变琥珀色。
  if (model.speedValid) {
    char latency[24];
    snprintf(latency, sizeof(latency), "LATENCY %u MS",
             static_cast<unsigned>(model.speedLatencyMs));
    updateLabel(m_speedLatencyLabel, latency);
    lv_obj_set_style_text_color(m_speedLatencyLabel,
                                model.speedLatencyMs > 500 ? kAmber : kFaint, 0);
  } else {
    updateLabel(m_speedLatencyLabel, "LATENCY -- MS");
  }

  const char gears[] = {'P', 'R', 'N', 'D'};
  bool selectedGearVisible = false;
  for (uint8_t i = 0; i < 4; ++i) {
    const bool selected = model.gear == gears[i];
    if (selected) {
      lv_obj_set_pos(m_gearHighlight, 152 + i * 42, 251);
      lv_obj_set_hidden(m_gearHighlight, false);
      selectedGearVisible = true;
    }
    lv_obj_set_style_text_color(m_gearLabels[i],
                                selected ? kScreen : lv_color_hex(0x5A6370), 0);
  }
  if (!selectedGearVisible) {
    lv_obj_set_hidden(m_gearHighlight, true);
  }

  if (model.powerValid) {
    char powerText[20];
    snprintf(powerText, sizeof(powerText), "%s%.1f kW",
             model.motorPowerKw < 0 ? "-" : "",
             fabsf(model.motorPowerKw));
    updateLabel(m_powerLabel, powerText);
    const bool regen = model.motorPowerKw < 0;
    const float maxKw = regen ? kPowerRegenMaxKw : kPowerOutMaxKw;
    const int32_t fillWidth = static_cast<int32_t>(
        fminf(fabsf(model.motorPowerKw), maxKw) * kPowerHalfWidthPx / maxKw);
    lv_obj_set_size(m_powerFill, fillWidth, 5);
    lv_obj_set_pos(m_powerFill, regen ? 134 - fillWidth : 134, 0);
    lv_obj_set_style_bg_color(m_powerFill, regen ? kGreen : kRed, 0);
  } else {
    updateLabel(m_powerLabel, "--.- kW");
    lv_obj_set_size(m_powerFill, 0, 5);
  }

  if (model.socValid) {
    char soc[12];
    snprintf(soc, sizeof(soc), "%d%%", static_cast<int>(model.soc));
    updateLabel(m_socLabel, soc);
    const int32_t width =
        model.soc < 0 ? 0 : (model.soc > 100 ? 120 : model.soc * 120 / 100);
    lv_obj_set_size(m_socBar, width, 3);
    const lv_color_t socColor = model.soc <= 15 ? kRed : kGaugeColor;
    lv_obj_set_style_text_color(m_socLabel, socColor, 0);
    lv_obj_set_style_bg_color(m_socBar, socColor, 0);
    lv_obj_set_size(
        m_socBarFill,
        model.soc < 0 ? 0
                      : (model.soc > 100 ? 280 : model.soc * 280 / 100),
        6);
    lv_obj_set_style_bg_color(m_socBarFill, kGreen, 0);
  } else {
    updateLabel(m_socLabel, "--%");
    lv_obj_set_size(m_socBar, 0, 3);
    lv_obj_set_size(m_socBarFill, 0, 6);
  }

  if (model.rangeValid) {
    char range[20];
    snprintf(range, sizeof(range), "%ld km",
             static_cast<long>(model.rangeKm));
    updateLabel(m_rangeLabel, range);
  } else {
    updateLabel(m_rangeLabel, "-- km");
  }

  if (model.temperatureValid) {
    char outsideTemp[16];
    char insideTemp[16];
    snprintf(outsideTemp, sizeof(outsideTemp), "%.0f°C",
             model.outsideTempC);
    snprintf(insideTemp, sizeof(insideTemp), "%.0f°C",
             model.insideTempC);
    updateLabel(m_outsideTempLabel, outsideTemp);
    updateLabel(m_insideTempLabel, insideTemp);
  } else {
    updateLabel(m_outsideTempLabel, "--°C");
    updateLabel(m_insideTempLabel, "--°C");
  }
}
