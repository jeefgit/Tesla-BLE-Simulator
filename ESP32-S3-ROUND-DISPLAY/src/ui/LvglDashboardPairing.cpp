#include "ui/LvglDashboard.h"

#include <stdio.h>

#include "ui/DashboardUi.h"

using namespace dashui;

namespace {

constexpr uint32_t kPairingWindowMs = 30000;

}  // namespace

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

void LvglDashboard::renderPairingPage(const UiModel& model) {
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
