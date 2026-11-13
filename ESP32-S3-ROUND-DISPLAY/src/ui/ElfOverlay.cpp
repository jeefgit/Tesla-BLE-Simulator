#include "ui/ElfOverlay.h"

#include <Arduino.h>
#include <esp_system.h>

#include "config.h"

#if CFG_ELF_ENABLE

namespace {

// 全部坐标直接使用 466×466 的 LVGL 画面坐标（见 design/ELF-DESIGN.md §3.2）。
constexpr int32_t kScreenSize = 466;

const lv_color_t kElfBg = lv_color_hex(0x020406);
const lv_color_t kSclera = lv_color_hex(0xE8F7FF);
const lv_color_t kIris = lv_color_hex(0x1C4E63);
const lv_color_t kPupil = lv_color_hex(0x04070A);
const lv_color_t kGlow = lv_color_hex(0x43D9F5);
const lv_color_t kBlush = lv_color_hex(0xFF8A80);
const lv_color_t kSleep = lv_color_hex(0x5A6377);

constexpr int32_t kEyeW = 128;
constexpr int32_t kEyeH = 150;
constexpr int32_t kEyeY = 133;
constexpr int32_t kEyeCenterY = kEyeY + kEyeH / 2;  // 208
constexpr int32_t kEyeX[2] = {90, 248};
constexpr int32_t kBlushX[2] = {103, 329};
constexpr int32_t kIrisSize = 92;
constexpr int32_t kIrisInset = (kEyeW - kIrisSize) / 2;  // 18
constexpr int32_t kIrisY = (kEyeH - kIrisSize) / 2;      // 29
constexpr int32_t kPupilSize = 74;
constexpr int32_t kPupilInset = (kIrisSize - kPupilSize) / 2;  // 9

// 眼睛闭合时的最小高度（视觉上是一条线）。
constexpr int32_t kEyeClosedH = 10;
// 眨眼节奏：闭 90ms / 停 40ms / 睁 140ms。
constexpr uint32_t kBlinkCloseMs = 90;
constexpr uint32_t kBlinkHoldMs = 40;
constexpr uint32_t kBlinkOpenMs = 140;
constexpr uint32_t kBlinkSecondGapMs = 140;
constexpr uint32_t kWakeMs = 220;
constexpr uint32_t kHideMs = 120;
constexpr uint32_t kGazeMs = 350;
// 呼吸：+2 → -2px，3s，再回来 3s，共 6s；让整屏轻微漂移以防烧屏。
constexpr int32_t kBreathOffsetPx = 2;
constexpr uint32_t kBreathHalfMs = 3000;

constexpr int32_t kGazeRangeX = 22;
constexpr int32_t kGazeRangeY = 14;

int32_t randomBetween(int32_t low, int32_t high) {
  if (high <= low) {
    return low;
  }
  return low + static_cast<int32_t>(
                   esp_random() % static_cast<uint32_t>(high - low + 1));
}

bool due(uint32_t at) { return static_cast<int32_t>(millis() - at) >= 0; }

// 建一个纯色几何块：无边框、不可点击、不滚动、不参与布局。
lv_obj_t* makeBlock(lv_obj_t* parent, int32_t x, int32_t y, int32_t w,
                    int32_t h, int32_t radius, lv_color_t color, uint8_t opa,
                    bool clipCorner = false) {
  lv_obj_t* obj = lv_obj_create(parent);
  lv_obj_remove_style_all(obj);
  lv_obj_set_pos(obj, x, y);
  lv_obj_set_size(obj, w, h);
  lv_obj_set_style_radius(obj, radius, 0);
  lv_obj_set_style_bg_color(obj, color, 0);
  lv_obj_set_style_bg_opa(obj, opa, 0);
  lv_obj_set_style_border_width(obj, 0, 0);
  lv_obj_set_style_pad_all(obj, 0, 0);
  lv_obj_set_style_clip_corner(obj, clipCorner, 0);
  lv_obj_set_scrollable(obj, false);
  lv_obj_set_clickable(obj, false);
  return obj;
}

lv_anim_t* startAnim(void* var, lv_anim_exec_xcb_t execCb, int32_t from,
                     int32_t to, uint32_t durationMs,
                     lv_anim_path_cb_t pathCb,
                     lv_anim_ready_cb_t readyCb) {
  lv_anim_t anim;
  lv_anim_init(&anim);
  lv_anim_set_var(&anim, var);
  lv_anim_set_exec_cb(&anim, execCb);
  lv_anim_set_values(&anim, from, to);
  lv_anim_set_duration(&anim, durationMs);
  lv_anim_set_path_cb(&anim, pathCb);
  if (readyCb != nullptr) {
    lv_anim_set_ready_cb(&anim, readyCb);
  }
  return lv_anim_start(&anim);
}

}  // namespace

void ElfOverlay::begin() {
  if (m_built) {
    return;
  }
  buildObjects();
  m_timer = lv_timer_create(timerCallback, 50, this);
  m_built = true;
  CFG_LOG("[ELF] overlay built, heap %u", ESP.getFreeHeap());
}

void ElfOverlay::buildObjects() {
  lv_obj_t* screen = lv_screen_active();

  // 根容器：不透明纯黑（AMOLED 关闭像素），盖住下面的仪表页；
  // 不可点击、默认隐藏。
  m_root = makeBlock(screen, 0, 0, kScreenSize, kScreenSize, 0, kElfBg,
                     LV_OPA_COVER);
  lv_obj_add_flag(m_root, LV_OBJ_FLAG_HIDDEN);

  // 装饰弧：R210、1px、8% 青色，用大圆边框代替阴影。
  m_aura = makeBlock(m_root, 23, 23, 420, 420, LV_RADIUS_CIRCLE,
                     lv_color_black(), LV_OPA_TRANSP);
  lv_obj_set_style_border_width(m_aura, 1, 0);
  lv_obj_set_style_border_color(m_aura, kGlow, 0);
  lv_obj_set_style_border_opa(m_aura, 20, 0);  // 约 8%

  for (int i = 0; i < 2; ++i) {
    // 眼球：胶囊形，子对象随高度压缩被裁掉，形成“合眼”。
    m_eye[i] = makeBlock(m_root, kEyeX[i], kEyeY, kEyeW, kEyeH, 64, kSclera,
                         LV_OPA_COVER, true);

    m_iris[i] = makeBlock(m_eye[i], kIrisInset, kIrisY, kIrisSize, kIrisSize,
                          LV_RADIUS_CIRCLE, kIris, LV_OPA_COVER);
    m_pupil[i] = makeBlock(m_iris[i], kPupilInset, kPupilInset, kPupilSize,
                           kPupilSize, LV_RADIUS_CIRCLE, kPupil, LV_OPA_COVER);
    // 高光挂在虹膜下：瞳孔放大时位置稳定，不会跟着漂。
    makeBlock(m_iris[i], 15, 15, 26, 26, LV_RADIUS_CIRCLE, lv_color_white(),
              230);
    makeBlock(m_iris[i], 58, 56, 12, 12, LV_RADIUS_CIRCLE, lv_color_white(),
              140);

    // 腮红：低不透明度胶囊，默认几乎看不见。
    m_blush[i] = makeBlock(m_root, kBlushX[i], 298, 34, 16, 8, kBlush, 31);
  }

  // 嘴：默认隐藏，开心时再出现。
  m_mouth = makeBlock(m_root, 211, 342, 44, 14, 7, kSleep, 77);
  lv_obj_add_flag(m_mouth, LV_OBJ_FLAG_HIDDEN);

  // 初始为闭眼状态，第一次 show() 才有“睁开”的过程。
  setEyeOpen(this, kEyeClosedH);
}

bool ElfOverlay::visible() const {
#if CFG_ELF_ENABLE
  return m_state != State::Hidden;
#else
  return false;
#endif
}

void ElfOverlay::show() {
  if (!m_built || m_state == State::Waking || m_state == State::Idle ||
      m_state == State::Blinking) {
    return;
  }
  // 从 Hidden 或 Hiding 进入：清掉残留动画，从当前高度睁眼。
  lv_anim_delete(this, setEyeOpen);
  lv_anim_delete(this, setGazeX);
  lv_anim_delete(this, setGazeY);
  lv_obj_clear_flag(m_root, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(m_root);
  m_state = State::Waking;
  m_blinkPhase = BlinkPhase::None;
  m_blinkIsSecond = false;

  int32_t currentH = lv_obj_get_height(m_eye[0]);
  if (currentH < kEyeClosedH) {
    currentH = kEyeClosedH;
  }
  setEyeOpen(this, currentH);
  startBreathing();
  animateEyes(currentH, kEyeH, kWakeMs, lv_anim_path_ease_out, onWakeDone);
  CFG_LOG("[ELF] show (wake from %d)", static_cast<int>(currentH));
}

void ElfOverlay::hide() {
  if (!m_built || m_state == State::Hidden || m_state == State::Hiding) {
    return;
  }
  lv_anim_delete(this, setEyeOpen);
  lv_anim_delete(this, setGazeX);
  lv_anim_delete(this, setGazeY);
  m_state = State::Hiding;
  m_blinkPhase = BlinkPhase::None;

  int32_t currentH = lv_obj_get_height(m_eye[0]);
  if (currentH < kEyeClosedH) {
    currentH = kEyeClosedH;
  }
  animateEyes(currentH, kEyeClosedH, kHideMs, lv_anim_path_ease_in,
              onHideDone);
  CFG_LOG("[ELF] hide (close from %d)", static_cast<int>(currentH));
}

void ElfOverlay::animateEyes(int32_t fromHeight, int32_t toHeight,
                             uint32_t durationMs, lv_anim_path_cb_t path,
                             lv_anim_ready_cb_t ready) {
  // var 用 this：一次动画同时驱动两只眼睛，也方便 lv_anim_delete 清理。
  startAnim(this, setEyeOpen, fromHeight, toHeight, durationMs, path, ready);
}

void ElfOverlay::startBreathing() {
  lv_anim_delete(m_root, setRootY);
  lv_obj_set_y(m_root, kBreathOffsetPx);
  lv_anim_t anim;
  lv_anim_init(&anim);
  lv_anim_set_var(&anim, m_root);
  lv_anim_set_exec_cb(&anim, setRootY);
  lv_anim_set_values(&anim, kBreathOffsetPx, -kBreathOffsetPx);
  lv_anim_set_duration(&anim, kBreathHalfMs);
  lv_anim_set_reverse_duration(&anim, kBreathHalfMs);
  lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
  lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
  lv_anim_start(&anim);
}

void ElfOverlay::stopBreathing() {
  lv_anim_delete(m_root, setRootY);
  lv_obj_set_y(m_root, 0);
}

void ElfOverlay::startBlink() {
  if (m_state != State::Idle) {
    return;
  }
  m_state = State::Blinking;
  m_blinkPhase = BlinkPhase::Closing;
  animateEyes(kEyeH, kEyeClosedH, kBlinkCloseMs, lv_anim_path_ease_in,
              onBlinkClosed);
  CFG_LOG("[ELF] blink%s", m_blinkIsSecond ? " x2" : "");
}

void ElfOverlay::startGaze() {
  if (m_state != State::Idle) {
    return;
  }
  const int32_t offsetX = randomBetween(-kGazeRangeX, kGazeRangeX);
  const int32_t offsetY = randomBetween(-kGazeRangeY, kGazeRangeY);
  const int32_t targetX = kIrisInset + offsetX;
  const int32_t targetY = kIrisY + offsetY;
  const int32_t fromX = lv_obj_get_x(m_iris[0]);
  const int32_t fromY = lv_obj_get_y(m_iris[0]);

  startAnim(this, setGazeX, fromX, targetX, kGazeMs, lv_anim_path_ease_out,
            nullptr);
  startAnim(this, setGazeY, fromY, targetY, kGazeMs, lv_anim_path_ease_out,
            nullptr);

  m_nextGazeAt = millis() + randomBetween(CFG_ELF_GAZE_MIN_MS,
                                          CFG_ELF_GAZE_MAX_MS);
  CFG_LOG("[ELF] gaze -> (%d,%d)", static_cast<int>(offsetX),
          static_cast<int>(offsetY));
}

void ElfOverlay::onTimer() {
  if (!m_built) {
    return;
  }
  const uint32_t now = millis();

  // 眨眼“停 40ms”阶段结束后睁眼。
  if (m_state == State::Blinking && m_blinkPhase == BlinkPhase::Holding &&
      now - m_blinkClosedAt >= kBlinkHoldMs) {
    m_blinkPhase = BlinkPhase::Opening;
    animateEyes(kEyeClosedH, kEyeH, kBlinkOpenMs, lv_anim_path_ease_out,
                onBlinkOpened);
    return;
  }

  if (m_state != State::Idle) {
    return;
  }
  if (due(m_nextBlinkAt)) {
    startBlink();
  } else if (due(m_nextGazeAt)) {
    startGaze();
  }
}

void ElfOverlay::lookAt(int16_t screenX, int16_t screenY) {
  // 阶段 3 实现：眼睛追随手指。
  (void)screenX;
  (void)screenY;
}

void ElfOverlay::poke(int16_t screenX, int16_t screenY) {
  // 阶段 3 实现：开心反应。
  (void)screenX;
  (void)screenY;
}

// ---------------------------------------------------------------------------
// 静态回调
// ---------------------------------------------------------------------------

void ElfOverlay::timerCallback(lv_timer_t* timer) {
  auto* self = static_cast<ElfOverlay*>(lv_timer_get_user_data(timer));
  if (self != nullptr) {
    self->onTimer();
  }
}

void ElfOverlay::setEyeOpen(void* var, int32_t height) {
  auto* self = static_cast<ElfOverlay*>(var);
  if (self == nullptr) {
    return;
  }
  if (height < kEyeClosedH) {
    height = kEyeClosedH;
  }
  // 高度压缩、中心不变：像眼皮合上，而不是眼睛整体下沉。
  const int32_t y = kEyeCenterY - height / 2;
  for (int i = 0; i < 2; ++i) {
    lv_obj_set_y(self->m_eye[i], y);
    lv_obj_set_height(self->m_eye[i], height);
  }
}

void ElfOverlay::setGazeX(void* var, int32_t x) {
  auto* self = static_cast<ElfOverlay*>(var);
  if (self == nullptr) {
    return;
  }
  for (int i = 0; i < 2; ++i) {
    lv_obj_set_x(self->m_iris[i], x);
  }
}

void ElfOverlay::setGazeY(void* var, int32_t y) {
  auto* self = static_cast<ElfOverlay*>(var);
  if (self == nullptr) {
    return;
  }
  for (int i = 0; i < 2; ++i) {
    lv_obj_set_y(self->m_iris[i], y);
  }
}

void ElfOverlay::setRootY(void* var, int32_t y) {
  lv_obj_set_y(static_cast<lv_obj_t*>(var), y);
}

void ElfOverlay::onWakeDone(lv_anim_t* anim) {
  auto* self = static_cast<ElfOverlay*>(anim->var);
  if (self == nullptr || self->m_state != State::Waking) {
    return;
  }
  self->m_state = State::Idle;
  const uint32_t now = millis();
  self->m_nextBlinkAt =
      now + randomBetween(CFG_ELF_BLINK_MIN_MS, CFG_ELF_BLINK_MAX_MS);
  self->m_nextGazeAt =
      now + randomBetween(CFG_ELF_GAZE_MIN_MS, CFG_ELF_GAZE_MAX_MS);
}

void ElfOverlay::onHideDone(lv_anim_t* anim) {
  auto* self = static_cast<ElfOverlay*>(anim->var);
  if (self == nullptr || self->m_state != State::Hiding) {
    return;
  }
  self->m_state = State::Hidden;
  self->m_blinkPhase = BlinkPhase::None;
  self->stopBreathing();
  lv_obj_add_flag(self->m_root, LV_OBJ_FLAG_HIDDEN);
}

void ElfOverlay::onBlinkClosed(lv_anim_t* anim) {
  auto* self = static_cast<ElfOverlay*>(anim->var);
  if (self == nullptr || self->m_state != State::Blinking) {
    return;
  }
  self->m_blinkClosedAt = millis();
  self->m_blinkPhase = BlinkPhase::Holding;
}

void ElfOverlay::onBlinkOpened(lv_anim_t* anim) {
  auto* self = static_cast<ElfOverlay*>(anim->var);
  if (self == nullptr || self->m_state != State::Blinking) {
    return;
  }
  self->m_state = State::Idle;
  self->m_blinkPhase = BlinkPhase::None;
  const uint32_t now = millis();
  // 20% 概率立刻再眨一下；第二下之后一定回到正常间隔，避免无限连眨。
  if (!self->m_blinkIsSecond && randomBetween(0, 99) < 20) {
    self->m_blinkIsSecond = true;
    self->m_nextBlinkAt = now + kBlinkSecondGapMs;
    self->m_nextGazeAt =
        now + randomBetween(CFG_ELF_GAZE_MIN_MS, CFG_ELF_GAZE_MAX_MS);
    return;
  }
  self->m_blinkIsSecond = false;
  self->m_nextBlinkAt =
      now + randomBetween(CFG_ELF_BLINK_MIN_MS, CFG_ELF_BLINK_MAX_MS);
  self->m_nextGazeAt =
      now + randomBetween(CFG_ELF_GAZE_MIN_MS, CFG_ELF_GAZE_MAX_MS);
}

#else  // CFG_ELF_ENABLE == 0

void ElfOverlay::begin() {}
bool ElfOverlay::visible() const { return false; }
void ElfOverlay::show() {}
void ElfOverlay::hide() {}
void ElfOverlay::lookAt(int16_t, int16_t) {}
void ElfOverlay::poke(int16_t, int16_t) {}

#endif  // CFG_ELF_ENABLE
