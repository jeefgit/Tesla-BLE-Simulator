#include "ui/DashboardUi.h"

namespace dashui {

const lv_color_t kScreen = lv_color_hex(0x020406);
const lv_color_t kText = lv_color_hex(0xF5F7FA);
const lv_color_t kDim = lv_color_hex(0x8A93A6);
const lv_color_t kFaint = lv_color_hex(0x5A6377);
const lv_color_t kRed = lv_color_hex(0xE82127);
const lv_color_t kAlert = lv_color_hex(0xFF453A);
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
                    int32_t height, int32_t radius) {
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
      return kAmber;
    case UiLinkState::Error:
      return kRed;
    case UiLinkState::Idle:
      return kFaint;
  }
  return kFaint;
}

}  // namespace dashui
