#include "ui/LvglDashboard.h"

#include <stdio.h>

#include "ui/DashboardUi.h"

using namespace dashui;

void LvglDashboard::buildDoorPage() {
  lv_obj_t* screen = lv_screen_active();
  m_doorPage = lv_obj_create(screen);
  lv_obj_remove_style_all(m_doorPage);
  lv_obj_set_size(m_doorPage, kScreenSize, kScreenSize);
  lv_obj_set_scrollable(m_doorPage, false);

  m_doorTitle = makeLabel(m_doorPage, 28, 84, 410, 25, "DOOR OPEN",
                          &lv_font_montserrat_20, kAlert,
                          LV_TEXT_ALIGN_CENTER);
  m_doorCount = makeLabel(m_doorPage, 28, 109, 410, 18,
                          "1 OPEN  /  5 CLOSED", &lv_font_montserrat_10,
                          kDim, LV_TEXT_ALIGN_CENTER);

  static const int16_t calloutPositions[6][2] = {
      {108, 214}, {342, 214}, {97, 295},
      {353, 295}, {225, 122}, {225, 383}};
  static const int16_t calloutBars[6][4] = {
      {124, 218, 48, 1}, {294, 218, 48, 1}, {113, 299, 49, 1},
      {304, 299, 49, 1}, {232, 132, 2, 12}, {232, 377, 2, 7}};

  for (uint8_t i = 0; i < 6; ++i) {
    lv_obj_t* bar = lv_obj_create(m_doorPage);
    lv_obj_remove_style_all(bar);
    lv_obj_set_pos(bar, calloutBars[i][0], calloutBars[i][1]);
    lv_obj_set_size(bar, calloutBars[i][2], calloutBars[i][3]);
    lv_obj_set_style_bg_color(bar, kFaint, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    m_doorCallouts[i] = bar;
  }

  lv_obj_t* body = makePanel(m_doorPage, 169, 137, 128, 246, 48);
  lv_obj_set_style_bg_color(body, lv_color_hex(0x080B0F), 0);
  lv_obj_set_style_border_color(body, lv_color_hex(0x656A72), 0);
  lv_obj_set_style_border_width(body, 2, 0);

  lv_obj_t* hood = makePanel(body, 15, 7, 96, 46, 28);
  lv_obj_set_style_bg_color(hood, lv_color_hex(0x20232A), 0);
  lv_obj_t* windshield = makePanel(body, 13, 57, 100, 54, 18);
  lv_obj_set_style_bg_color(windshield, lv_color_hex(0x0D1116), 0);
  lv_obj_t* roof = makePanel(body, 13, 117, 100, 65, 16);
  lv_obj_set_style_bg_color(roof, lv_color_hex(0x101419), 0);
  lv_obj_t* rearWindow = makePanel(body, 13, 188, 100, 34, 14);
  lv_obj_set_style_bg_color(rearWindow, lv_color_hex(0x0D1116), 0);
  lv_obj_t* trunk = makePanel(body, 15, 226, 96, 12, 6);
  lv_obj_set_style_bg_color(trunk, lv_color_hex(0x20232A), 0);

  static const int16_t panelPositions[4][4] = {
      {160, 194, 12, 48}, {294, 194, 12, 48},
      {160, 251, 12, 48}, {294, 251, 12, 48}};
  for (uint8_t i = 0; i < 4; ++i) {
    lv_obj_t* panel = lv_obj_create(m_doorPage);
    lv_obj_remove_style_all(panel);
    lv_obj_set_pos(panel, panelPositions[i][0], panelPositions[i][1]);
    lv_obj_set_size(panel, panelPositions[i][2], panelPositions[i][3]);
    lv_obj_set_style_radius(panel, 5, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x22262D), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x656A72), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_transform_pivot_x(panel, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(panel, lv_pct(50), 0);
    lv_obj_set_clickable(panel, false);
    m_doorPanels[i] = panel;
  }

  for (uint8_t i = 0; i < 6; ++i) {
    lv_obj_t* indicator = lv_obj_create(m_doorPage);
    lv_obj_remove_style_all(indicator);
    lv_obj_set_pos(indicator, calloutPositions[i][0],
                   calloutPositions[i][1]);
    lv_obj_set_size(indicator, 16, 16);
    lv_obj_set_style_radius(indicator, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(indicator, lv_color_hex(0x030604), 0);
    lv_obj_set_style_bg_opa(indicator, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(indicator, kGreen, 0);
    lv_obj_set_style_border_width(indicator, 1, 0);
    lv_obj_set_clickable(indicator, false);
    m_doorIndicators[i] = indicator;

    lv_obj_t* center = lv_obj_create(indicator);
    lv_obj_remove_style_all(center);
    lv_obj_set_size(center, 6, 6);
    lv_obj_center(center);
    lv_obj_set_style_radius(center, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(center, kGreen, 0);
    lv_obj_set_style_bg_opa(center, LV_OPA_COVER, 0);
    m_doorIndicatorCenters[i] = center;
  }

  makeLabel(m_doorPage, 28, 427, 410, 18,
            "AUTO-RETURNS WHEN ALL CLOSED", &lv_font_montserrat_10, kFaint,
            LV_TEXT_ALIGN_CENTER);
}

void LvglDashboard::renderDoorPage(const UiModel& model, uint8_t openDoorCount,
                                   int8_t firstOpenDoor) {
  static const char* doorNames[6] = {
      "FRONT LEFT DOOR OPEN", "FRONT RIGHT DOOR OPEN",
      "REAR LEFT DOOR OPEN", "REAR RIGHT DOOR OPEN",
      "FRUNK OPEN", "TRUNK OPEN"};
  char countText[32];
  snprintf(countText, sizeof(countText), "%u OPEN  /  %u CLOSED",
           static_cast<unsigned>(openDoorCount),
           static_cast<unsigned>(6 - openDoorCount));
  updateLabel(m_doorTitle, doorNames[firstOpenDoor]);
  updateLabel(m_doorCount, countText);

  uint8_t doorMask = 0;
  for (uint8_t i = 0; i < 6; ++i) {
    if (model.doors[i]) {
      doorMask |= static_cast<uint8_t>(1u << i);
    }
  }
  if (doorMask != m_doorDisplayMask) {
    m_doorDisplayMask = doorMask;
    for (uint8_t i = 0; i < 6; ++i) {
      const bool open = (doorMask & (1u << i)) != 0;
      const lv_color_t stateColor = open ? kAlert : kGreen;
      lv_obj_set_style_border_color(m_doorIndicators[i], stateColor, 0);
      lv_obj_set_style_bg_color(m_doorIndicatorCenters[i], stateColor, 0);
      lv_obj_set_style_bg_color(m_doorCallouts[i],
                                open ? kAlert : kFaint, 0);
      if (i < 4) {
        const bool left = (i % 2) == 0;
        const int32_t x = open ? (left ? 146 : 308) : (left ? 160 : 294);
        const int32_t y = i < 2 ? 194 : 251;
        lv_obj_set_pos(m_doorPanels[i], x, y);
        lv_obj_set_style_transform_rotation(
            m_doorPanels[i], open ? (left ? 300 : 3300) : 0, 0);
        lv_obj_set_style_bg_color(m_doorPanels[i],
                                  open ? lv_color_hex(0x381319)
                                       : lv_color_hex(0x22262D),
                                  0);
        lv_obj_set_style_border_color(m_doorPanels[i],
                                      open ? kAlert
                                           : lv_color_hex(0x656A72),
                                      0);
      }
    }
  }
}
