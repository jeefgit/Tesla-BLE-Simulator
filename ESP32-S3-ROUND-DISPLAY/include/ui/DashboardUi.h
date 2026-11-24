#pragma once

#include <lvgl.h>

#include <stdint.h>

#include "core/UiModel.h"

// Shared palette and widget factories for the LVGL dashboard pages.
// Each page lives in its own .cpp; everything they have in common is declared
// here and defined in src/ui/DashboardUi.cpp.
namespace dashui {

constexpr int32_t kScreenSize = 466;

extern const lv_color_t kScreen;
extern const lv_color_t kText;
extern const lv_color_t kDim;
extern const lv_color_t kFaint;
extern const lv_color_t kRed;
extern const lv_color_t kAlert;
extern const lv_color_t kGreen;
extern const lv_color_t kAmber;
extern const lv_color_t kTrack;
extern const lv_color_t kGaugeColor;
extern const lv_color_t kCard;
extern const lv_color_t kBootRingColor;
extern const lv_color_t kScanCyan;

lv_obj_t* makeLabel(lv_obj_t* parent, int32_t x, int32_t y, int32_t width,
                    int32_t height, const char* text, const lv_font_t* font,
                    lv_color_t color, lv_text_align_t align);

lv_obj_t* makePanel(lv_obj_t* parent, int32_t x, int32_t y, int32_t width,
                    int32_t height, int32_t radius = 14);

lv_obj_t* makeScanArc(lv_obj_t* parent, int32_t size, int32_t width,
                      int32_t x, int32_t y, lv_color_t color,
                      int32_t value);

void rotateScanArc(void* object, int32_t angle);

void setScanPulseOpacity(void* object, int32_t opacity);

void startScanArcAnimation(lv_obj_t* arc, int32_t start, int32_t end,
                           uint32_t duration);

void setScale(lv_obj_t* object, int32_t scale);

void setPairCardPulse(void* object, int32_t scale);

lv_obj_t* makeScaledLabel(lv_obj_t* parent, int32_t centerX, int32_t centerY,
                          const char* text, const lv_font_t* font,
                          lv_color_t color, int32_t scale);

const char* linkText(UiLinkState state);

lv_color_t linkColor(UiLinkState state);

}  // namespace dashui
