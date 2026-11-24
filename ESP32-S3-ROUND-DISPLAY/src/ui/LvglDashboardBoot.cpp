#include "ui/LvglDashboard.h"

#include <stdio.h>

#include "config.h"
#include "fw_version_api.h"
#include "ui/DashboardUi.h"

extern const lv_image_dsc_t teslaBootLogo;

using namespace dashui;

namespace {

constexpr uint32_t kBootRingDurationMs = 1500;

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
  UI_LOG("boot logo animation start");
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
    UI_LOG("boot ring animation start");
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
    UI_LOG("boot ring %lums fill=%ld%%",
            static_cast<unsigned long>(elapsed),
            static_cast<long>(s_bootPercent));
    s_bootNextLogMs = elapsed + 250;
  }

  if (s_bootPercent >= 100) {
    lv_timer_delete(timer);
    UI_LOG("boot ring animation finished");
    lv_obj_t* layer = lv_obj_get_parent(ring);
    lv_timer_create(bootLogoFinished, kBootLogoHoldMs, layer);
  }
}

}  // namespace

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

  // Firmware version, static at the bottom of the boot screen (no animation).
  char verText[32];
  snprintf(verText, sizeof(verText), "v%s", fwVersion());
  makeLabel(m_bootLayer, 0, 406, kScreenSize, 24, verText,
            &lv_font_montserrat_18, kDim, LV_TEXT_ALIGN_CENTER);

  // Start the elapsed-time animation on the first lv_timer_handler() run.
  s_bootPercent = 0;
  s_bootStartMs = 0;
  s_bootNextLogMs = 0;
  s_bootStarted = false;
  s_bootRunning = true;
  lv_timer_create(bootRingTick, kBootRingTickMs, m_bootRing);
}

bool LvglDashboard::bootAnimationRunning() const { return s_bootRunning; }
