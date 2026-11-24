#include "ui/LvglDashboard.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "config/VinStore.h"
#include "ui/DashboardUi.h"

using namespace dashui;

namespace {

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

}  // namespace

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
