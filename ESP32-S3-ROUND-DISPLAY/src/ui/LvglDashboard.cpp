#include "ui/LvglDashboard.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "config.h"

extern const lv_image_dsc_t teslaBootLogo;

namespace {

constexpr int32_t kScreenSize = 466;
constexpr uint32_t kPairingWindowMs = 30000;
constexpr int32_t kArcMaxValue = 1000;
constexpr float kGaugeMaxSpeedKmh = 240.0f;
constexpr uint32_t kBootRingDurationMs = 1500;
const lv_color_t kScreen = lv_color_hex(0x020406);
const lv_color_t kText = lv_color_hex(0xF5F7FA);
const lv_color_t kDim = lv_color_hex(0x8A93A6);
const lv_color_t kFaint = lv_color_hex(0x5A6377);
const lv_color_t kRed = lv_color_hex(0xE82127);
const lv_color_t kGreen = lv_color_hex(0x21C15E);
const lv_color_t kAmber = lv_color_hex(0xFFB340);
const lv_color_t kTrack = lv_color_hex(0x1D2733);
const lv_color_t kGaugeColor = lv_color_hex(0x43D9F5);
const lv_color_t kCard = lv_color_hex(0x0C1218);
const lv_color_t kBootRingColor = lv_color_hex(0xAEB8C2);
const lv_color_t kScanCyan = lv_color_hex(0x43D9F5);

lv_obj_t* makeLabel(lv_obj_t* parent, int32_t x, int32_t y, int32_t width,
                    int32_t height, const char* text, const lv_font_t* font,
                    lv_color_t color, lv_text_align_t align) {
  lv_obj_t* label = lv_label_create(parent);
  lv_label_set_text(label, text);
  lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
  lv_obj_set_pos(label, x, y);
  lv_obj_set_size(label, width, height);
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_set_style_text_color(label, color, 0);
  lv_obj_set_style_text_align(label, align, 0);
  lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(label, 0, 0);
  lv_obj_set_style_pad_all(label, 0, 0);
  return label;
}

lv_obj_t* makePanel(lv_obj_t* parent, int32_t x, int32_t y, int32_t width,
                    int32_t height, int32_t radius = 14) {
  lv_obj_t* panel = lv_obj_create(parent);
  lv_obj_remove_style_all(panel);
  lv_obj_set_pos(panel, x, y);
  lv_obj_set_size(panel, width, height);
  lv_obj_set_style_radius(panel, radius, 0);
  lv_obj_set_style_bg_color(panel, kCard, 0);
  lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(panel, kTrack, 0);
  lv_obj_set_style_border_width(panel, 1, 0);
  lv_obj_set_style_pad_all(panel, 0, 0);
  lv_obj_set_scrollable(panel, false);
  return panel;
}

lv_obj_t* makeScanArc(lv_obj_t* parent, int32_t size, int32_t width,
                      int32_t x, int32_t y, lv_color_t color,
                      int32_t value) {
  lv_obj_t* arc = lv_arc_create(parent);
  lv_obj_remove_style_all(arc);
  lv_obj_set_pos(arc, x, y);
  lv_obj_set_size(arc, size, size);
  lv_arc_set_rotation(arc, 0);
  lv_arc_set_bg_angles(arc, 0, 360);
  lv_arc_set_range(arc, 0, 100);
  lv_arc_set_value(arc, value);
  lv_arc_set_mode(arc, LV_ARC_MODE_NORMAL);
  lv_obj_set_clickable(arc, false);
  lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(arc, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(arc, 0, LV_PART_MAIN);
  lv_obj_set_style_arc_width(arc, 2, LV_PART_MAIN);
  lv_obj_set_style_arc_color(arc, lv_color_hex(0x172734), LV_PART_MAIN);
  lv_obj_set_style_arc_width(arc, width, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(arc, color, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
  lv_obj_set_style_border_width(arc, 0, LV_PART_KNOB);
  lv_obj_set_style_pad_all(arc, 0, LV_PART_KNOB);
  return arc;
}

void rotateScanArc(void* object, int32_t angle) {
  lv_arc_set_rotation(static_cast<lv_obj_t*>(object), angle);
}

void setScanPulseOpacity(void* object, int32_t opacity) {
  lv_obj_set_style_opa(static_cast<lv_obj_t*>(object), opacity, 0);
}

void startScanArcAnimation(lv_obj_t* arc, int32_t start, int32_t end,
                           uint32_t duration) {
  lv_anim_t animation;
  lv_anim_init(&animation);
  lv_anim_set_var(&animation, arc);
  lv_anim_set_values(&animation, start, end);
  lv_anim_set_duration(&animation, duration);
  lv_anim_set_path_cb(&animation, lv_anim_path_linear);
  lv_anim_set_exec_cb(&animation, rotateScanArc);
  lv_anim_set_repeat_count(&animation, LV_ANIM_REPEAT_INFINITE);
  lv_anim_start(&animation);
}

void setScale(lv_obj_t* object, int32_t scale) {
  // Use percentage pivots: they are resolved at render time from the final
  // object size. lv_obj_get_width()/height() would still report the pre-layout
  // size here, which moved scaled labels far to the right/top.
  lv_obj_set_style_transform_pivot_x(object, lv_pct(50), 0);
  lv_obj_set_style_transform_pivot_y(object, lv_pct(50), 0);
  lv_obj_set_style_transform_scale(object, scale, 0);
}

void setPairCardPulse(void* object, int32_t scale) {
  auto* frame = static_cast<lv_obj_t*>(object);
  setScale(frame, scale);
  const int32_t opacity = 45 + (scale - 240) * 55 / 100;
  lv_obj_set_style_opa(frame, opacity, 0);
}

// A scaled label whose box is exactly one line high. The text then fills the
// box, so scaling around the 50%/50% pivot keeps it centered on both axes.
lv_obj_t* makeScaledLabel(lv_obj_t* parent, int32_t centerX, int32_t centerY,
                          const char* text, const lv_font_t* font,
                          lv_color_t color, int32_t scale) {
  const int32_t lineHeight = lv_font_get_line_height(font);
  // 宽度用内容宽度，而不是整屏 466。
  // LVGL 给带 transform 的对象分配 ARGB8888 图层时按对象框大小分配；
  // 整屏宽 466x56 约 120KB，会把 LVGL 堆撑爆（曾导致 StoreProhibited）。
  // 按内容宽度后图层一般只有几十 KB，同时每帧要变换的像素也少很多。
  lv_obj_t* label =
      makeLabel(parent, 0, 0, LV_SIZE_CONTENT, lineHeight, text, font, color,
                LV_TEXT_ALIGN_CENTER);
  lv_obj_align(label, LV_ALIGN_TOP_MID, centerX - kScreenSize / 2,
               centerY - lineHeight / 2);
  setScale(label, scale);
  return label;
}

const char* linkText(UiLinkState state) {
  switch (state) {
    case UiLinkState::Idle:
      return "IDLE";
    case UiLinkState::Scanning:
      return "OFFLINE";
    case UiLinkState::Connecting:
      return "LINKING";
    case UiLinkState::Syncing:
      return "SYNCING";
    case UiLinkState::Ready:
      return "LINK";
    case UiLinkState::Reconnecting:
      return "RETRY";
    case UiLinkState::Error:
      return "ERROR";
  }
  return "OFFLINE";
}

lv_color_t linkColor(UiLinkState state) {
  switch (state) {
    case UiLinkState::Ready:
      return kGreen;
    case UiLinkState::Scanning:
    case UiLinkState::Connecting:
    case UiLinkState::Syncing:
      return kAmber;
    case UiLinkState::Reconnecting:
    case UiLinkState::Error:
      return kRed;
    case UiLinkState::Idle:
      return kFaint;
  }
  return kFaint;
}

// ---- boot animation -------------------------------------------------------
// Circular progress fill on the outermost circle of the round panel: the full
// circle is drawn as a dim track, then a silver ring fills it clockwise from
// 12 o'clock. The filled share follows elapsed time until the circle is 100 %
// full, then the overlay is removed so the dashboard becomes visible.
//
// The timer only controls how often the arc is refreshed. Progress is based on
// elapsed time so a delayed LVGL frame doesn't lengthen the animation.
constexpr uint32_t kBootRingTickMs = 10;
constexpr uint32_t kBootLogoHoldMs = 1000;
constexpr uint32_t kBootLogoAnimMs = kBootRingDurationMs + 500;
constexpr int32_t kBootLogoStartScale = 64;
constexpr int32_t kBootLogoEndScale = 320;
constexpr int32_t kBootRingSize = 460;
constexpr int32_t kBootRingWidth = 12;
// Start at 12 o'clock: LVGL's 0 degree is 3 o'clock and angles grow clockwise,
// so a 270 degree rotation moves the arc's start to the top of the circle.
constexpr int32_t kBootRingRotation = 270;

int32_t s_bootPercent = 0;
uint32_t s_bootStartMs = 0;
uint32_t s_bootNextLogMs = 0;
bool s_bootStarted = false;
bool s_bootRunning = false;
lv_obj_t* s_bootEmblem = nullptr;

void bootLogoAnimate(void* object, int32_t scale) {
  auto* emblem = static_cast<lv_obj_t*>(object);
  setScale(emblem, scale);
  const int32_t opacity =
      (scale - kBootLogoStartScale) * LV_OPA_COVER /
      (kBootLogoEndScale - kBootLogoStartScale);
  lv_obj_set_style_opa(emblem, opacity, 0);
}

void startBootLogoAnimation() {
  if (s_bootEmblem == nullptr) return;

  lv_anim_t animation;
  lv_anim_init(&animation);
  lv_anim_set_var(&animation, s_bootEmblem);
  lv_anim_set_values(&animation, kBootLogoStartScale, kBootLogoEndScale);
  lv_anim_set_duration(&animation, kBootLogoAnimMs);
  lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&animation, bootLogoAnimate);
  lv_anim_start(&animation);
  CFG_LOG("[UI] boot logo animation start");
}

void bootLogoFinished(lv_timer_t* timer) {
  auto* layer = static_cast<lv_obj_t*>(lv_timer_get_user_data(timer));
  lv_timer_delete(timer);
  if (layer != nullptr) {
    lv_obj_delete(layer);
  }
  s_bootEmblem = nullptr;
  s_bootRunning = false;
}

void bootRingTick(lv_timer_t* timer) {
  auto* ring = static_cast<lv_obj_t*>(lv_timer_get_user_data(timer));
  if (ring == nullptr) {
    lv_timer_delete(timer);
    return;
  }

  if (!s_bootStarted) {
    s_bootStartMs = millis();
    s_bootNextLogMs = 0;
    s_bootStarted = true;
    CFG_LOG("[UI] boot ring animation start");
    startBootLogoAnimation();
  }

  const uint32_t elapsed = millis() - s_bootStartMs;
  s_bootPercent = elapsed >= kBootRingDurationMs
                      ? 100
                      : static_cast<int32_t>(
                            elapsed * 100 / kBootRingDurationMs);
  // Range is 0..100, so the value is literally "how much of the circle is
  // filled". LVGL maps it onto the 0..360 background arc.
  lv_arc_set_value(ring, s_bootPercent);

  if (elapsed >= s_bootNextLogMs) {
    CFG_LOG("[UI] boot ring %lums fill=%ld%%",
            static_cast<unsigned long>(elapsed),
            static_cast<long>(s_bootPercent));
    s_bootNextLogMs = elapsed + 250;
  }

  if (s_bootPercent >= 100) {
    lv_timer_delete(timer);
    CFG_LOG("[UI] boot ring animation finished");
    lv_obj_t* layer = lv_obj_get_parent(ring);
    lv_timer_create(bootLogoFinished, kBootLogoHoldMs, layer);
  }
}

}  // namespace

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
  buildBootAnimation();
}

void LvglDashboard::buildBootAnimation() {
  lv_obj_t* screen = lv_screen_active();

  // Full-screen opaque layer on top of the dashboard. It hides the pages until
  // the ring and logo sequence finishes.
  m_bootLayer = lv_obj_create(screen);
  lv_obj_remove_style_all(m_bootLayer);
  lv_obj_set_pos(m_bootLayer, 0, 0);
  lv_obj_set_size(m_bootLayer, kScreenSize, kScreenSize);
  lv_obj_set_style_bg_color(m_bootLayer, kScreen, 0);
  lv_obj_set_style_bg_opa(m_bootLayer, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(m_bootLayer, 0, 0);
  lv_obj_set_scrollable(m_bootLayer, false);
  lv_obj_set_clickable(m_bootLayer, false);
  lv_obj_move_foreground(m_bootLayer);

  // Circular progress ring at the outermost edge of the round panel: the full
  // circle is a dim track, the purple indicator fills it clockwise from 12
  // o'clock as the percentage grows.
  constexpr int32_t ringOffset = (kScreenSize - kBootRingSize) / 2;
  m_bootRing = lv_arc_create(m_bootLayer);
  lv_obj_remove_style_all(m_bootRing);
  lv_obj_set_size(m_bootRing, kBootRingSize, kBootRingSize);
  lv_obj_set_pos(m_bootRing, ringOffset, ringOffset);
  lv_arc_set_rotation(m_bootRing, kBootRingRotation);
  lv_arc_set_bg_angles(m_bootRing, 0, 360);
  lv_arc_set_range(m_bootRing, 0, 100);
  lv_arc_set_value(m_bootRing, 0);
  lv_arc_set_mode(m_bootRing, LV_ARC_MODE_NORMAL);
  lv_obj_set_clickable(m_bootRing, false);

  lv_obj_set_style_bg_opa(m_bootRing, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(m_bootRing, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(m_bootRing, 0, LV_PART_MAIN);
  lv_obj_set_style_arc_width(m_bootRing, kBootRingWidth, LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(m_bootRing, true, LV_PART_MAIN);
  lv_obj_set_style_arc_color(m_bootRing, kTrack, LV_PART_MAIN);

  lv_obj_set_style_arc_width(m_bootRing, kBootRingWidth, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(m_bootRing, true, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(m_bootRing, kBootRingColor,
                             LV_PART_INDICATOR);

  // No knob: the arc is display-only.
  lv_obj_set_style_bg_opa(m_bootRing, LV_OPA_TRANSP, LV_PART_KNOB);
  lv_obj_set_style_border_width(m_bootRing, 0, LV_PART_KNOB);
  lv_obj_set_style_pad_all(m_bootRing, 0, LV_PART_KNOB);

  m_bootEmblem = lv_image_create(m_bootLayer);
  lv_obj_remove_style_all(m_bootEmblem);
  lv_image_set_src(m_bootEmblem, &teslaBootLogo);
  lv_obj_set_pos(m_bootEmblem, (kScreenSize - 128) / 2,
                 (kScreenSize - 163) / 2);
  lv_obj_set_style_bg_opa(m_bootEmblem, LV_OPA_TRANSP, 0);
  lv_obj_set_style_opa(m_bootEmblem, LV_OPA_TRANSP, 0);
  s_bootEmblem = m_bootEmblem;

  // Start the elapsed-time animation on the first lv_timer_handler() run.
  s_bootPercent = 0;
  s_bootStartMs = 0;
  s_bootNextLogMs = 0;
  s_bootStarted = false;
  s_bootRunning = true;
  lv_timer_create(bootRingTick, kBootRingTickMs, m_bootRing);
}

bool LvglDashboard::bootAnimationRunning() const { return s_bootRunning; }

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

  m_gaugeMinLabel = makeLabel(screen, 47, 150, 30, 18, "0",
                              &lv_font_montserrat_10, kFaint,
                              LV_TEXT_ALIGN_CENTER);
  m_gaugeMaxLabel = makeLabel(screen, 389, 150, 30, 18, "240",
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

  m_speedLabel = makeScaledLabel(m_speedGroup, 233, 163, "--",
                                 &lv_font_montserrat_48, kText, 600);
  makeLabel(m_speedGroup, 0, 235, 466, 18, "KM/H",
            &lv_font_montserrat_14, kGaugeColor, LV_TEXT_ALIGN_CENTER);

  const char* gears[] = {"P", "R", "N", "D"};
  const int32_t gearX[] = {176, 206, 236, 266};
  m_gearHighlight = lv_obj_create(m_speedGroup);
  lv_obj_remove_style_all(m_gearHighlight);
  lv_obj_set_size(m_gearHighlight, 30, 30);
  lv_obj_set_style_radius(m_gearHighlight, 9, 0);
  lv_obj_set_style_bg_color(m_gearHighlight, kGaugeColor, 0);
  lv_obj_set_style_bg_opa(m_gearHighlight, LV_OPA_COVER, 0);
  lv_obj_set_hidden(m_gearHighlight, true);

  for (int32_t i = 0; i < 4; ++i) {
    m_gearLabels[i] = makeLabel(m_speedGroup, gearX[i], 258, 24, 24,
                                gears[i], &lv_font_montserrat_20, kFaint,
                                LV_TEXT_ALIGN_CENTER);
  }

  makeLabel(m_speedGroup, 83, 291, 300, 14, "MOTOR POWER",
            &lv_font_montserrat_10, kFaint, LV_TEXT_ALIGN_CENTER);
  m_powerLabel = makeLabel(m_speedGroup, 73, 303, 320, 33, "--.- kW",
                           &lv_font_montserrat_28, kText, LV_TEXT_ALIGN_CENTER);

  lv_obj_t* powerTrack = lv_obj_create(m_speedGroup);
  lv_obj_remove_style_all(powerTrack);
  lv_obj_set_pos(powerTrack, 73, 341);
  lv_obj_set_size(powerTrack, 320, 6);
  lv_obj_set_style_radius(powerTrack, 3, 0);
  lv_obj_set_style_bg_color(powerTrack, lv_color_hex(0x17232C), 0);
  lv_obj_set_style_bg_opa(powerTrack, LV_OPA_COVER, 0);

  lv_obj_t* zero = lv_obj_create(powerTrack);
  lv_obj_remove_style_all(zero);
  lv_obj_set_pos(zero, 159, -2);
  lv_obj_set_size(zero, 2, 10);
  lv_obj_set_style_bg_color(zero, lv_color_hex(0x637987), 0);
  lv_obj_set_style_bg_opa(zero, LV_OPA_COVER, 0);

  m_powerFill = lv_obj_create(powerTrack);
  lv_obj_remove_style_all(m_powerFill);
  lv_obj_set_pos(m_powerFill, 160, 0);
  lv_obj_set_size(m_powerFill, 0, 6);
  lv_obj_set_style_radius(m_powerFill, 3, 0);
  lv_obj_set_style_bg_color(m_powerFill, kRed, 0);
  lv_obj_set_style_bg_opa(m_powerFill, LV_OPA_COVER, 0);
}

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

void LvglDashboard::buildCards() {
  lv_obj_t* screen = lv_screen_active();
  m_cardsGroup = lv_obj_create(screen);
  lv_obj_remove_style_all(m_cardsGroup);
  lv_obj_set_size(m_cardsGroup, 466, 466);
  lv_obj_set_scrollable(m_cardsGroup, false);

  lv_obj_t* battery = makePanel(m_cardsGroup, 83, 344, 144, 56);
  lv_obj_set_style_border_color(battery, lv_color_hex(0x24414B), 0);
  lv_obj_t* batteryAccent = lv_obj_create(battery);
  lv_obj_remove_style_all(batteryAccent);
  lv_obj_set_pos(batteryAccent, 12, 8);
  lv_obj_set_size(batteryAccent, 3, 12);
  lv_obj_set_style_radius(batteryAccent, 2, 0);
  lv_obj_set_style_bg_color(batteryAccent, kGaugeColor, 0);
  lv_obj_set_style_bg_opa(batteryAccent, LV_OPA_COVER, 0);
  makeLabel(battery, 21, 8, 111, 12, "BATTERY",
            &lv_font_montserrat_10, kFaint, LV_TEXT_ALIGN_LEFT);
  m_socLabel = makeLabel(battery, 12, 19, 120, 27, "--%",
                         &lv_font_montserrat_24, kText, LV_TEXT_ALIGN_LEFT);

  lv_obj_t* meterTrack = lv_obj_create(battery);
  lv_obj_remove_style_all(meterTrack);
  lv_obj_set_pos(meterTrack, 12, 48);
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

  lv_obj_t* range = makePanel(m_cardsGroup, 239, 344, 144, 56);
  lv_obj_set_style_border_color(range, lv_color_hex(0x49391F), 0);
  lv_obj_t* rangeAccent = lv_obj_create(range);
  lv_obj_remove_style_all(rangeAccent);
  lv_obj_set_pos(rangeAccent, 12, 8);
  lv_obj_set_size(rangeAccent, 3, 12);
  lv_obj_set_style_radius(rangeAccent, 2, 0);
  lv_obj_set_style_bg_color(rangeAccent, kAmber, 0);
  lv_obj_set_style_bg_opa(rangeAccent, LV_OPA_COVER, 0);
  makeLabel(range, 21, 8, 111, 12, "EST. RANGE",
            &lv_font_montserrat_10, kFaint, LV_TEXT_ALIGN_LEFT);
  m_rangeLabel = makeLabel(range, 12, 19, 120, 27, "-- km",
                           &lv_font_montserrat_24, kText, LV_TEXT_ALIGN_LEFT);

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

  lv_obj_t* outsideTemp = makePanel(m_temperatureGroup, 83, 406, 144, 42, 10);
  makeLabel(outsideTemp, 10, 5, 124, 12, "OUTSIDE",
            &lv_font_montserrat_10, kFaint, LV_TEXT_ALIGN_CENTER);
  m_outsideTempLabel = makeLabel(outsideTemp, 10, 19, 124, 20, "--°C",
                                 &lv_font_montserrat_16, kText,
                                 LV_TEXT_ALIGN_CENTER);

  lv_obj_t* insideTemp = makePanel(m_temperatureGroup, 239, 406, 144, 42, 10);
  makeLabel(insideTemp, 10, 5, 124, 12, "CABIN",
            &lv_font_montserrat_10, kFaint, LV_TEXT_ALIGN_CENTER);
  m_insideTempLabel = makeLabel(insideTemp, 10, 19, 124, 20, "--°C",
                                &lv_font_montserrat_16, kText,
                                LV_TEXT_ALIGN_CENTER);
}

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

void LvglDashboard::buildPairingPage() {
  lv_obj_t* screen = lv_screen_active();
  m_pairPage = lv_obj_create(screen);
  lv_obj_remove_style_all(m_pairPage);
  lv_obj_set_size(m_pairPage, 466, 466);
  lv_obj_set_scrollable(m_pairPage, false);

  makeLabel(m_pairPage, 35, 66, 396, 18, "ADD NEW KEY",
            &lv_font_montserrat_10, kFaint, LV_TEXT_ALIGN_CENTER);
  m_pairTitle = makeScaledLabel(m_pairPage, 233, 119, "TAP CARD",
                                &lv_font_montserrat_48, kAmber, 260);
  makeLabel(m_pairPage, 32, 154, 402, 20,
            "HOLD CARD NEAR THE CONSOLE READER", &lv_font_montserrat_10, kDim,
            LV_TEXT_ALIGN_CENTER);

  m_pairCardGlow = lv_obj_create(m_pairPage);
  lv_obj_remove_style_all(m_pairCardGlow);
  lv_obj_set_pos(m_pairCardGlow, 176, 196);
  lv_obj_set_size(m_pairCardGlow, 114, 68);
  lv_obj_set_style_radius(m_pairCardGlow, 14, 0);
  lv_obj_set_style_border_width(m_pairCardGlow, 1, 0);
  lv_obj_set_style_border_color(m_pairCardGlow, kAmber, 0);
  lv_obj_set_style_bg_opa(m_pairCardGlow, LV_OPA_TRANSP, 0);
  lv_obj_set_clickable(m_pairCardGlow, false);

  lv_obj_t* card = lv_obj_create(m_pairPage);
  lv_obj_remove_style_all(card);
  lv_obj_set_pos(card, 183, 201);
  lv_obj_set_size(card, 100, 58);
  lv_obj_set_style_radius(card, 10, 0);
  lv_obj_set_style_bg_color(card, lv_color_hex(0x171811), 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_border_color(card, kAmber, 0);
  lv_obj_set_clickable(card, false);

  lv_obj_t* chip = lv_obj_create(card);
  lv_obj_remove_style_all(chip);
  lv_obj_set_pos(chip, 12, 14);
  lv_obj_set_size(chip, 18, 24);
  lv_obj_set_style_radius(chip, 4, 0);
  lv_obj_set_style_bg_color(chip, lv_color_hex(0x392D17), 0);
  lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(chip, 1, 0);
  lv_obj_set_style_border_color(chip, kAmber, 0);

  lv_obj_t* cardStripe = lv_obj_create(card);
  lv_obj_remove_style_all(cardStripe);
  lv_obj_set_pos(cardStripe, 39, 17);
  lv_obj_set_size(cardStripe, 43, 3);
  lv_obj_set_style_radius(cardStripe, 2, 0);
  lv_obj_set_style_bg_color(cardStripe, lv_color_hex(0x695127), 0);
  lv_obj_set_style_bg_opa(cardStripe, LV_OPA_COVER, 0);
  lv_obj_t* cardStripe2 = lv_obj_create(card);
  lv_obj_remove_style_all(cardStripe2);
  lv_obj_set_pos(cardStripe2, 39, 26);
  lv_obj_set_size(cardStripe2, 32, 2);
  lv_obj_set_style_radius(cardStripe2, 1, 0);
  lv_obj_set_style_bg_color(cardStripe2, lv_color_hex(0x695127), 0);
  lv_obj_set_style_bg_opa(cardStripe2, LV_OPA_COVER, 0);

  m_pairPulse = lv_obj_create(m_pairPage);
  lv_obj_remove_style_all(m_pairPulse);
  lv_obj_set_size(m_pairPulse, 8, 8);
  lv_obj_set_pos(m_pairPulse, 256, 226);
  lv_obj_set_style_radius(m_pairPulse, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(m_pairPulse, kScanCyan, 0);
  lv_obj_set_style_bg_opa(m_pairPulse, LV_OPA_COVER, 0);
  lv_obj_set_clickable(m_pairPulse, false);
  makeLabel(m_pairPage, 179, 269, 108, 16, "NFC  READY",
            &lv_font_montserrat_10, kScanCyan, LV_TEXT_ALIGN_CENTER);

  m_pairCount = makeScaledLabel(m_pairPage, 233, 315, "30",
                                &lv_font_montserrat_48, kText, 440);

  lv_obj_t* bar = lv_obj_create(m_pairPage);
  lv_obj_remove_style_all(bar);
  lv_obj_set_pos(bar, 93, 367);
  lv_obj_set_size(bar, 280, 7);
  lv_obj_set_style_radius(bar, 4, 0);
  lv_obj_set_style_bg_color(bar, kTrack, 0);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
  m_pairBarFill = lv_obj_create(bar);
  lv_obj_remove_style_all(m_pairBarFill);
  lv_obj_set_pos(m_pairBarFill, 0, 0);
  lv_obj_set_size(m_pairBarFill, 280, 7);
  lv_obj_set_style_radius(m_pairBarFill, 4, 0);
  lv_obj_set_style_bg_color(m_pairBarFill, kAmber, 0);
  lv_obj_set_style_bg_opa(m_pairBarFill, LV_OPA_COVER, 0);

  m_pairTry = makeLabel(m_pairPage, 50, 384, 366, 16, "TRY 1 / 3",
                        &lv_font_montserrat_10, kFaint,
                        LV_TEXT_ALIGN_CENTER);
  m_pairStatus = makeLabel(m_pairPage, 32, 403, 402, 20,
                           "WAITING FOR KEY CARD",
                           &lv_font_montserrat_10, kAmber,
                           LV_TEXT_ALIGN_CENTER);
  makeLabel(m_pairPage, 30, 428, 406, 16,
            "WINDOW CLOSES AUTOMATICALLY",
            &lv_font_montserrat_10, kFaint, LV_TEXT_ALIGN_CENTER);
}

void LvglDashboard::startPairAnimation() {
  if (m_pairAnimationRunning) return;
  m_pairAnimationRunning = true;

  lv_anim_t pulse;
  lv_anim_init(&pulse);
  lv_anim_set_var(&pulse, m_pairPulse);
  lv_anim_set_values(&pulse, 70, LV_OPA_COVER);
  lv_anim_set_duration(&pulse, 650);
  lv_anim_set_playback_time(&pulse, 650);
  lv_anim_set_exec_cb(&pulse, setScanPulseOpacity);
  lv_anim_set_repeat_count(&pulse, LV_ANIM_REPEAT_INFINITE);
  lv_anim_start(&pulse);

  lv_anim_t cardPulse;
  lv_anim_init(&cardPulse);
  lv_anim_set_var(&cardPulse, m_pairCardGlow);
  lv_anim_set_values(&cardPulse, 240, 340);
  lv_anim_set_duration(&cardPulse, 850);
  lv_anim_set_playback_time(&cardPulse, 850);
  lv_anim_set_exec_cb(&cardPulse, setPairCardPulse);
  lv_anim_set_repeat_count(&cardPulse, LV_ANIM_REPEAT_INFINITE);
  lv_anim_start(&cardPulse);
}

void LvglDashboard::stopPairAnimation() {
  if (!m_pairAnimationRunning) return;
  lv_anim_delete(m_pairPulse, setScanPulseOpacity);
  lv_anim_delete(m_pairCardGlow, setPairCardPulse);
  m_pairAnimationRunning = false;
  lv_obj_set_style_opa(m_pairPulse, LV_OPA_COVER, 0);
  setPairCardPulse(m_pairCardGlow, 240);
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
  int32_t speed = 0;
  if (valid) {
    speed = static_cast<int32_t>(
        lroundf(fminf(fmaxf(speedKmh, 0.0f), kGaugeMaxSpeedKmh)));
  }
  // Map the rounded km/h (the same number shown on screen) to arc units, so the
  // arc only invalidates when the visible speed actually changes instead of on
  // every smoothed float step.
  const int32_t value = speed * kArcMaxValue /
                        static_cast<int32_t>(kGaugeMaxSpeedKmh);
  lv_arc_set_value(m_arc, value);
}

void LvglDashboard::render(const UiModel& model) {
  const bool pairing =
      model.link == UiLinkState::Ready &&
      model.pairing != UiPairingPhase::Idle;
  const bool ready = model.link == UiLinkState::Ready;
  const bool showLinkPage = !pairing && !ready;
  const bool showCharging = ready && model.charging && !pairing;

  setHidden(m_speedGroup, !ready || showCharging || pairing);
  setHidden(m_chargeGroup, !showCharging);
  setHidden(m_cardsGroup, !ready || showCharging || pairing);
  setHidden(m_chargeCardsGroup, !showCharging);
  setHidden(m_temperatureGroup, !ready || pairing);
  setHidden(m_arc, showLinkPage || pairing);
  setHidden(m_gaugeMinLabel, pairing);
  setHidden(m_gaugeMaxLabel, pairing);
  setHidden(m_topGroup, pairing);
  setHidden(m_linkPage, !showLinkPage);
  setHidden(m_pairPage, !pairing);
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

  updateLabel(m_gaugeMinLabel, "0");
  updateLabel(m_gaugeMaxLabel, showCharging ? "100" : "240");
  const lv_color_t currentLinkColor = linkColor(model.link);
  lv_obj_set_style_bg_color(m_linkDot, currentLinkColor, 0);
  lv_obj_set_style_text_color(m_linkLabel, currentLinkColor, 0);
  updateLabel(m_linkLabel, pairing ? "PAIRING" : linkText(model.link));
  lv_obj_set_style_text_color(m_lockLabel,
                              model.locked ? kFaint : kAmber, 0);
  updateLabel(m_lockLabel,
              pairing ? "KEY" : (model.locked ? "LOCK" : "UNLOCK"));

  m_showSpeedArc = ready && !pairing && !showCharging;
  if (pairing) {
    lv_obj_set_style_arc_color(m_arc, kAmber, LV_PART_INDICATOR);
    lv_arc_set_value(m_arc, kArcMaxValue);
  } else if (showCharging) {
    lv_obj_set_style_arc_color(m_arc, kGreen, LV_PART_INDICATOR);
    lv_arc_set_value(m_arc, model.socValid ? model.soc * 10 : 0);
  } else if (m_showSpeedArc) {
    lv_obj_set_style_arc_color(m_arc, kGaugeColor, LV_PART_INDICATOR);
    // The value is applied by updateSpeedLabel() at the end of render(), which
    // uses the same smoothed speed as the numeric readout. Updating here too
    // would invalidate the arc twice per frame.
  } else {
    lv_arc_set_value(m_arc, 0);
  }
  lv_obj_set_style_arc_color(m_arc,
                             ready ? kTrack : lv_color_hex(0x141C25),
                             LV_PART_MAIN);

  updateSpeedLabel(model.speedKmh, model.speedValid);

  const char gears[] = {'P', 'R', 'N', 'D'};
  bool selectedGearVisible = false;
  for (uint8_t i = 0; i < 4; ++i) {
    const bool selected = model.gear == gears[i];
    if (selected) {
      lv_obj_set_pos(m_gearHighlight, 173 + i * 30, 255);
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
    const int32_t fillWidth = static_cast<int32_t>(
        fminf(fabsf(model.motorPowerKw), 250.0f) * 0.64f);
    const bool regen = model.motorPowerKw < 0;
    lv_obj_set_size(m_powerFill, fillWidth, 6);
    lv_obj_set_pos(m_powerFill, regen ? 160 - fillWidth : 160, 0);
    lv_obj_set_style_bg_color(m_powerFill, regen ? kGreen : kRed, 0);
  } else {
    updateLabel(m_powerLabel, "--.- kW");
    lv_obj_set_size(m_powerFill, 0, 6);
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

  if (showCharging) {
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

  if (showLinkPage) {
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
  }

  if (pairing) {
    const char* title = "TAP CARD";
    const char* status = "WAITING FOR KEY CARD";
    switch (model.pairing) {
      case UiPairingPhase::TapCard:
        break;
      case UiPairingPhase::WaitConfirm:
        title = "CONFIRM";
        status = "CONFIRM ON VEHICLE SCREEN";
        break;
      case UiPairingPhase::Retry:
        title = "RETRY";
        status = "RETRYING KEY REQUEST";
        break;
      case UiPairingPhase::Success:
        title = "KEY ADDED";
        status = "PAIRING COMPLETE";
        break;
      case UiPairingPhase::Failed:
        title = "FAILED";
        status = "PAIRING NEEDS ATTENTION";
        break;
      case UiPairingPhase::Idle:
        break;
    }
    updateLabel(m_pairTitle, title);
    updateLabel(m_pairStatus, status);
    lv_obj_set_style_text_color(m_pairTitle,
                                model.pairing == UiPairingPhase::Failed
                                    ? kRed
                                    : kAmber,
                                0);
    char count[12];
    snprintf(count, sizeof(count), "%lu",
             static_cast<unsigned long>((model.pairingRemainMs + 999) / 1000));
    updateLabel(m_pairCount, count);
    char attempt[24];
    snprintf(attempt, sizeof(attempt), "TRY %u / %u",
             model.pairingAttempt, model.pairingMaxAttempts);
    updateLabel(m_pairTry, attempt);
    const uint32_t remain = model.pairingRemainMs > kPairingWindowMs
                                ? kPairingWindowMs
                                : model.pairingRemainMs;
    lv_obj_set_size(m_pairBarFill,
                    static_cast<int32_t>(
                        280U * remain / kPairingWindowMs),
                    7);
  }
}
