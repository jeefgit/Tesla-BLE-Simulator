#include "ui/LvglDashboard.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "fw_version_api.h"
#include "config/VinStore.h"

extern const lv_image_dsc_t teslaBootLogo;

namespace {

constexpr int32_t kScreenSize = 466;
constexpr uint32_t kPairingWindowMs = 30000;
constexpr int32_t kArcMaxValue = 1000;
constexpr float kGaugeMaxSpeedKmh = 150.0f;
// Motor power bar: the track is 268px wide with the zero mark at x=134, so
// each direction has 134px of travel. Asymmetric range: -100kW regen (green,
// left) and +150kW output (red, right), each scaled to fill its half.
constexpr float kPowerRegenMaxKw = 100.0f;
constexpr float kPowerOutMaxKw = 150.0f;
constexpr float kPowerHalfWidthPx = 134.0f;
constexpr uint32_t kBootRingDurationMs = 1500;
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

// VIN editor geometry. The 6x5 grid is centred on the 466px round screen.
constexpr int32_t kVinKeySize = 52;
constexpr int32_t kVinKeyPitch = 58;  // 52 + 6 gap
constexpr int32_t kVinGridCols = 6;
constexpr int32_t kVinGridRows = 5;
constexpr int32_t kVinGridX = 62;  // (466 - (5 * 58 + 52)) / 2
constexpr int32_t kVinGridY = 91;
constexpr int32_t kVinKeyCount = kVinGridCols * kVinGridRows;

// Key indices depend on the active layout, so never hard-code them.
constexpr int vinDelKeyIndex(bool numeric) { return numeric ? 10 : 26; }
constexpr int vinSwitchKeyIndex(bool numeric) { return numeric ? 11 : 27; }
constexpr int vinOkKeyIndex(bool numeric) { return numeric ? 12 : 28; }
constexpr int vinCancelKeyIndex(bool numeric) { return numeric ? 13 : 29; }

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

void LvglDashboard::buildVinEditor() {
  lv_obj_t* screen = lv_screen_active();

  // Full-screen opaque overlay created after the boot layer so it is always on
  // top. Hidden by default; touch is dispatched manually by DashboardApp.
  m_vinEditor = lv_obj_create(screen);
  lv_obj_remove_style_all(m_vinEditor);
  lv_obj_set_pos(m_vinEditor, 0, 0);
  lv_obj_set_size(m_vinEditor, kScreenSize, kScreenSize);
  lv_obj_set_style_bg_color(m_vinEditor, kScreen, 0);
  lv_obj_set_style_bg_opa(m_vinEditor, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(m_vinEditor, 0, 0);
  lv_obj_set_style_pad_all(m_vinEditor, 0, 0);
  lv_obj_set_scrollable(m_vinEditor, false);
  lv_obj_set_clickable(m_vinEditor, false);

  m_vinTitle = makeLabel(m_vinEditor, 0, 12, kScreenSize, 16, "VEHICLE VIN",
                         &lv_font_montserrat_10, kDim, LV_TEXT_ALIGN_CENTER);
  m_vinText = makeLabel(m_vinEditor, 0, 30, kScreenSize, 34, "--",
                        &lv_font_montserrat_24, kText, LV_TEXT_ALIGN_CENTER);
  m_vinCounter = makeLabel(m_vinEditor, 0, 63, kScreenSize, 16, "0 / 17",
                           &lv_font_montserrat_10, kDim,
                           LV_TEXT_ALIGN_CENTER);
  m_vinStatus =
      makeLabel(m_vinEditor, 0, 388, kScreenSize, 16, "ENTER 17 CHARACTERS",
                &lv_font_montserrat_10, kFaint, LV_TEXT_ALIGN_CENTER);

  const int32_t keyLineHeight = lv_font_get_line_height(&lv_font_montserrat_20);
  for (int32_t i = 0; i < kVinKeyCount; ++i) {
    const int32_t x = kVinGridX + (i % kVinGridCols) * kVinKeyPitch;
    const int32_t y = kVinGridY + (i / kVinGridCols) * kVinKeyPitch;
    m_vinKeys[i] = makePanel(m_vinEditor, x, y, kVinKeySize, kVinKeySize, 12);
    lv_obj_set_clickable(m_vinKeys[i], false);
    m_vinKeyLabels[i] =
        makeLabel(m_vinKeys[i], 0, (kVinKeySize - keyLineHeight) / 2,
                  kVinKeySize, keyLineHeight, "", &lv_font_montserrat_20, kText,
                  LV_TEXT_ALIGN_CENTER);
  }

  vinEditorSetLayout(false);
  lv_obj_set_hidden(m_vinEditor, true);
  m_vinEditorVisible = false;
}

bool LvglDashboard::bootAnimationRunning() const { return s_bootRunning; }

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

void LvglDashboard::vinEditorShow(const char* initialVin) {
  if (!m_vinEditor) return;

  // Keep only 17 uppercase A-Z0-9 characters; anything else is dropped.
  size_t len = 0;
  if (initialVin) {
    for (const char* p = initialVin;
         *p != '\0' && len < vinstore::kLength; ++p) {
      const char c =
          static_cast<char>(toupper(static_cast<unsigned char>(*p)));
      if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
        m_vinEdit[len++] = c;
      }
    }
  }
  m_vinEdit[len] = '\0';
  m_vinEditLen = static_cast<uint8_t>(len);

  m_vinNumericLayout = false;
  m_vinStatusText[0] = '\0';
  lv_obj_set_style_text_color(m_vinStatus, kDim, 0);
  vinEditorSetLayout(false);
  vinEditorRefresh();
  lv_obj_set_hidden(m_vinEditor, false);
  lv_obj_move_foreground(m_vinEditor);
  m_vinEditorVisible = true;
  CFG_LOG("[VIN] editor show len=%u", static_cast<unsigned>(m_vinEditLen));
}

void LvglDashboard::vinEditorHide() {
  m_vinEditorVisible = false;
  if (m_vinEditor) lv_obj_set_hidden(m_vinEditor, true);
  CFG_LOG("[VIN] editor hide");
  m_vinStatusText[0] = '\0';
  if (m_vinStatus) updateLabel(m_vinStatus, "");
}

void LvglDashboard::vinEditorSetLayout(bool numeric) {
  m_vinNumericLayout = numeric;
  for (int32_t i = 0; i < kVinKeyCount; ++i) {
    if (!m_vinKeys[i] || !m_vinKeyLabels[i]) continue;

    const char* text = "";
    char letter[2] = {};
    bool visible = true;
    if (numeric) {
      if (i <= 8) {
        letter[0] = static_cast<char>('1' + i);
        text = letter;
      } else if (i == 9) {
        text = "0";
      } else if (i == vinDelKeyIndex(true)) {
        text = "DEL";
      } else if (i == vinSwitchKeyIndex(true)) {
        text = "ABC";
      } else if (i == vinOkKeyIndex(true)) {
        text = "OK";
      } else if (i == vinCancelKeyIndex(true)) {
        text = "X";
      } else {
        visible = false;
      }
    } else {
      if (i <= 25) {
        letter[0] = static_cast<char>('A' + i);
        text = letter;
      } else if (i == vinDelKeyIndex(false)) {
        text = "DEL";
      } else if (i == vinSwitchKeyIndex(false)) {
        text = "123";
      } else if (i == vinOkKeyIndex(false)) {
        text = "OK";
      } else if (i == vinCancelKeyIndex(false)) {
        text = "X";
      } else {
        visible = false;
      }
    }

    updateLabel(m_vinKeyLabels[i], text);
    lv_obj_set_hidden(m_vinKeys[i], !visible);
    m_vinKeyVisible[i] = visible;
  }
}

void LvglDashboard::vinEditorRefresh() {
  if (!m_vinEditor) return;

  updateLabel(m_vinText, m_vinEditLen > 0 ? m_vinEdit : "--");

  char counter[16];
  snprintf(counter, sizeof(counter), "%u / %u",
           static_cast<unsigned>(m_vinEditLen),
           static_cast<unsigned>(vinstore::kLength));
  updateLabel(m_vinCounter, counter);

  // Default hint; an error set afterwards stays until the next key press.
  const bool ready = m_vinEditLen >= vinstore::kLength;
  const char* status =
      ready ? "READY - TYPE TO REPLACE" : "ENTER 17 CHARACTERS";
  snprintf(m_vinStatusText, sizeof(m_vinStatusText), "%s", status);
  updateLabel(m_vinStatus, m_vinStatusText);
  lv_obj_set_style_text_color(m_vinStatus, kDim, 0);

  const int okKey = vinOkKeyIndex(m_vinNumericLayout);
  lv_obj_set_style_text_color(m_vinKeyLabels[okKey], ready ? kText : kFaint, 0);
  lv_obj_set_style_border_color(m_vinKeys[okKey], ready ? kGreen : kTrack, 0);
}

int LvglDashboard::vinHitKey(int16_t x, int16_t y) const {
  if (!m_vinEditorVisible) return -1;
  // Use the whole 58px cell (52px key + 6px gap) so slightly-off taps still
  // register; adjacent cells just touch without overlapping.
  constexpr int32_t kCellPad = (kVinKeyPitch - kVinKeySize) / 2;
  for (int32_t i = 0; i < kVinKeyCount; ++i) {
    if (!m_vinKeyVisible[i]) continue;
    const int32_t keyX = kVinGridX + (i % kVinGridCols) * kVinKeyPitch;
    const int32_t keyY = kVinGridY + (i / kVinGridCols) * kVinKeyPitch;
    if (x >= keyX - kCellPad && x < keyX + kVinKeySize + kCellPad &&
        y >= keyY - kCellPad && y < keyY + kVinKeySize + kCellPad) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

bool LvglDashboard::vinEditorHandleTap(int16_t x, int16_t y) {
  if (!m_vinEditorVisible) return false;

  const int key = vinHitKey(x, y);
  CFG_LOG("[VIN-UI] tap x=%d y=%d key=%d layout=%d", x, y, key,
          m_vinNumericLayout ? 1 : 0);
  if (key < 0) return true;  // Consume taps inside the overlay.

  if (key == vinCancelKeyIndex(m_vinNumericLayout)) {
    if (m_vinCancelHandler) m_vinCancelHandler();
    return true;
  }
  if (key == vinOkKeyIndex(m_vinNumericLayout)) {
    vinEditorSubmitOrError();
    return true;
  }
  if (key == vinDelKeyIndex(m_vinNumericLayout)) {
    if (m_vinEditLen > 0) {
      m_vinEdit[--m_vinEditLen] = '\0';
      vinEditorRefresh();
    }
    return true;
  }
  if (key == vinSwitchKeyIndex(m_vinNumericLayout)) {
    vinEditorSetLayout(!m_vinNumericLayout);
    vinEditorRefresh();
    return true;
  }

  // Character keys: digits map differently from letters.
  const char ch = m_vinNumericLayout
                      ? (key <= 8 ? static_cast<char>('1' + key)
                                  : (key == 9 ? '0' : '\0'))
                      : (key <= 25 ? static_cast<char>('A' + key) : '\0');
  if (ch != '\0') {
    if (m_vinEditLen < vinstore::kLength) {
      m_vinEdit[m_vinEditLen++] = ch;
      m_vinEdit[m_vinEditLen] = '\0';
    } else {
      // The editor always starts pre-filled with the current 17-char VIN.
      // Typing must still give feedback, so drop the oldest character and
      // append the new one (the common "shift left" VIN/serial behaviour).
      memmove(m_vinEdit, m_vinEdit + 1, vinstore::kLength - 1);
      m_vinEdit[vinstore::kLength - 1] = ch;
      m_vinEdit[vinstore::kLength] = '\0';
    }
    vinEditorRefresh();
  }
  return true;
}

void LvglDashboard::vinEditorSetError(const char* text) {
  snprintf(m_vinStatusText, sizeof(m_vinStatusText), "%s", text ? text : "");
  updateLabel(m_vinStatus, m_vinStatusText);
  lv_obj_set_style_text_color(m_vinStatus, kRed, 0);
}

void LvglDashboard::vinEditorShowSaved() {
  snprintf(m_vinStatusText, sizeof(m_vinStatusText), "%s",
           "SAVED - RESTARTING...");
  updateLabel(m_vinStatus, m_vinStatusText);
  lv_obj_set_style_text_color(m_vinStatus, kGreen, 0);
}

void LvglDashboard::vinEditorSubmitOrError() {
  const vinstore::Error err = vinstore::validate(m_vinEdit);
  if (err != vinstore::Error::None) {
    vinEditorSetError(vinstore::errorText(err));
    return;
  }
  if (m_vinSubmitHandler) m_vinSubmitHandler(m_vinEdit);
}

void LvglDashboard::vinEditorSetSubmitHandler(
    std::function<void(const char*)> handler) {
  m_vinSubmitHandler = handler;
}

void LvglDashboard::vinEditorSetCancelHandler(
    std::function<void()> handler) {
  m_vinCancelHandler = handler;
}

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
  m_pairCard = card;
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
  m_pairChip = chip;
  lv_obj_set_pos(chip, 12, 14);
  lv_obj_set_size(chip, 18, 24);
  lv_obj_set_style_radius(chip, 4, 0);
  lv_obj_set_style_bg_color(chip, lv_color_hex(0x392D17), 0);
  lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(chip, 1, 0);
  lv_obj_set_style_border_color(chip, kAmber, 0);

  lv_obj_t* cardStripe = lv_obj_create(card);
  lv_obj_remove_style_all(cardStripe);
  m_pairStripe1 = cardStripe;
  lv_obj_set_pos(cardStripe, 39, 17);
  lv_obj_set_size(cardStripe, 43, 3);
  lv_obj_set_style_radius(cardStripe, 2, 0);
  lv_obj_set_style_bg_color(cardStripe, lv_color_hex(0x695127), 0);
  lv_obj_set_style_bg_opa(cardStripe, LV_OPA_COVER, 0);
  lv_obj_t* cardStripe2 = lv_obj_create(card);
  lv_obj_remove_style_all(cardStripe2);
  m_pairStripe2 = cardStripe2;
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
  m_pairNfcLabel = makeLabel(m_pairPage, 179, 269, 108, 16, "NFC  READY",
                             &lv_font_montserrat_10, kScanCyan,
                             LV_TEXT_ALIGN_CENTER);

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
  m_pairHint = makeLabel(m_pairPage, 30, 428, 406, 16,
                         "WINDOW CLOSES AUTOMATICALLY",
                         &lv_font_montserrat_10, kFaint, LV_TEXT_ALIGN_CENTER);

  // 成功态徽章：绿色圆环 + 对勾，位于原倒计时数字的位置，默认隐藏。
  m_pairSuccess = lv_obj_create(m_pairPage);
  lv_obj_remove_style_all(m_pairSuccess);
  lv_obj_set_pos(m_pairSuccess, 191, 273);
  lv_obj_set_size(m_pairSuccess, 84, 84);
  lv_obj_set_clickable(m_pairSuccess, false);
  lv_obj_add_flag(m_pairSuccess, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t* halo = lv_obj_create(m_pairSuccess);
  lv_obj_remove_style_all(halo);
  lv_obj_set_pos(halo, 3, 3);
  lv_obj_set_size(halo, 78, 78);
  lv_obj_set_style_radius(halo, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(halo, 1, 0);
  lv_obj_set_style_border_color(halo, kGreen, 0);
  lv_obj_set_style_border_opa(halo, LV_OPA_40, 0);
  lv_obj_set_style_bg_opa(halo, LV_OPA_TRANSP, 0);
  lv_obj_set_clickable(halo, false);

  lv_obj_t* ring = lv_obj_create(m_pairSuccess);
  lv_obj_remove_style_all(ring);
  lv_obj_set_pos(ring, 10, 10);
  lv_obj_set_size(ring, 64, 64);
  lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(ring, 2, 0);
  lv_obj_set_style_border_color(ring, kGreen, 0);
  lv_obj_set_style_bg_color(ring, lv_color_hex(0x0A2415), 0);
  lv_obj_set_style_bg_opa(ring, LV_OPA_COVER, 0);
  lv_obj_set_clickable(ring, false);

  // 对勾用一条折线画出；lv_line 只保存指针，数组必须静态存活。
  static const lv_point_precise_t checkPoints[] = {
      {0, 14}, {9, 23}, {28, 0},
  };
  lv_obj_t* check = lv_line_create(m_pairSuccess);
  lv_line_set_points(check, checkPoints, 3);
  lv_obj_set_pos(check, 28, 31);
  lv_obj_set_style_line_width(check, 5, 0);
  lv_obj_set_style_line_color(check, kGreen, 0);
  lv_obj_set_style_line_rounded(check, true, 0);
  lv_obj_set_clickable(check, false);
}

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
  updateLabel(m_gaugeMaxLabel, showCharging ? "100" : "150");
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
    // The value is applied by updateSpeedLabel() at the end of render(), which
    // uses the same smoothed speed as the numeric readout.
  } else {
    lv_arc_set_value(m_arc, 0);
  }
  lv_obj_set_style_arc_color(m_arc,
                             ready ? kTrack : lv_color_hex(0x141C25),
                             LV_PART_MAIN);

  updateSpeedLabel(model.speedKmh, model.speedValid);

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
    const bool pairSuccess = model.pairing == UiPairingPhase::Success;
    const bool pairFailed = model.pairing == UiPairingPhase::Failed;
    const lv_color_t pairAccent =
        pairFailed ? kRed : (pairSuccess ? kGreen : kAmber);
    lv_obj_set_style_text_color(m_pairTitle, pairAccent, 0);
    lv_obj_set_style_text_color(m_pairStatus, pairAccent, 0);

    // 卡片、芯片、读卡点随状态变色：等待=琥珀，成功=绿，失败=红。
    lv_obj_set_style_border_color(m_pairCardGlow, pairAccent, 0);
    lv_obj_set_style_border_color(m_pairCard, pairAccent, 0);
    lv_obj_set_style_border_color(m_pairChip, pairAccent, 0);
    lv_obj_set_style_bg_color(m_pairStripe1,
                              pairSuccess ? lv_color_hex(0x2E7A44)
                                          : lv_color_hex(0x695127),
                              0);
    lv_obj_set_style_bg_color(m_pairStripe2,
                              pairSuccess ? lv_color_hex(0x2E7A44)
                                          : lv_color_hex(0x695127),
                              0);
    lv_obj_set_style_bg_color(m_pairPulse, pairAccent, 0);

    if (pairSuccess) {
      lv_obj_clear_flag(m_pairSuccess, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(m_pairNfcLabel, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(m_pairTry, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(m_pairHint, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(m_pairSuccess, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(m_pairNfcLabel, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(m_pairTry, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(m_pairHint, LV_OBJ_FLAG_HIDDEN);
    }
    // 倒计时只在有效等待阶段显示；窗口结束或 Success/Failed 时 remainMs=0，
    // 会出现孤零零的大号 "0"，很难看，直接隐藏。
    const bool countVisible =
        model.pairing != UiPairingPhase::Success &&
        model.pairing != UiPairingPhase::Failed &&
        model.pairing != UiPairingPhase::Idle && model.pairingRemainMs > 0;
    if (countVisible) {
      char count[12];
      snprintf(count, sizeof(count), "%lu",
               static_cast<unsigned long>((model.pairingRemainMs + 999) / 1000));
      updateLabel(m_pairCount, count);
      lv_obj_clear_flag(m_pairCount, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(m_pairCount, LV_OBJ_FLAG_HIDDEN);
    }
    char attempt[24];
    snprintf(attempt, sizeof(attempt), "TRY %u / %u",
             model.pairingAttempt, model.pairingMaxAttempts);
    updateLabel(m_pairTry, attempt);
    // 成功时进度条保持满格绿色；其余阶段按剩余窗口缩短。
    if (pairSuccess) {
      lv_obj_set_style_bg_color(m_pairBarFill, kGreen, 0);
      lv_obj_set_size(m_pairBarFill, 280, 7);
    } else {
      lv_obj_set_style_bg_color(m_pairBarFill, kAmber, 0);
      const uint32_t remain = model.pairingRemainMs > kPairingWindowMs
                                  ? kPairingWindowMs
                                  : model.pairingRemainMs;
      lv_obj_set_size(m_pairBarFill,
                      static_cast<int32_t>(
                          280U * remain / kPairingWindowMs),
                      7);
    }
  }
}
