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
// 嘴巴常态可见；开心/打哈欠时更亮。
const lv_color_t kMouthColor = lv_color_hex(0x9FB4C4);
constexpr uint8_t kMouthIdleOpa = 120;
constexpr uint8_t kMouthHappyOpa = 235;
constexpr uint8_t kMouthYawnOpa = 235;

// 耳朵/尾巴轮廓：中暗青色，在纯黑背景上可见但不抢眼睛。
const lv_color_t kSilhouette = lv_color_hex(0x2A5A68);

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
constexpr int32_t kPupilHappySize = 82;
constexpr int32_t kPupilInset = (kIrisSize - kPupilSize) / 2;  // 9

// 阶段 4：车速越快瞳孔越大。20 km/h 起、100 km/h 到顶（74 → 86），
// 覆盖市区常用速度区间，变化更容易看出来。
constexpr int32_t kPupilSpeedMaxSize = 86;
constexpr float kPupilSpeedStartKmh = 20.0f;
constexpr float kPupilSpeedFullKmh = 100.0f;
constexpr uint32_t kPupilAnimMs = 300;

// 车速高于此值视为“正在驾驶”：保持清醒，不进入困倦/睡眠。
constexpr float kDrivingSpeedKmh = 5.0f;

// 急减速眯眼：一次采样内降幅 ≥ 10 km/h 触发，眯 600ms，2s 冷却。
constexpr int32_t kEyeSquintH = 62;
constexpr uint32_t kSquintInMs = 120;
constexpr uint32_t kSquintHoldMs = 600;
constexpr uint32_t kSquintOutMs = 240;
constexpr uint32_t kSquintCooldownMs = 2000;
constexpr float kDecelDropKmh = 10.0f;
constexpr uint32_t kDecelWindowMs = 450;

// 充电打哈欠：慢闭 500ms / 停 400ms / 慢睁 500ms。
constexpr uint32_t kYawnCloseMs = 500;
constexpr uint32_t kYawnHoldMs = 400;
constexpr uint32_t kYawnOpenMs = 500;
constexpr uint32_t kYawnFirstMinMs = 3000;
constexpr uint32_t kYawnFirstMaxMs = 6000;
constexpr uint32_t kYawnNextMinMs = 12000;
constexpr uint32_t kYawnNextMaxMs = 20000;

// 耳朵 / 尾巴剪影（挂在 m_body、眼睛之前，呼吸时一起漂移）。
constexpr int32_t kEarX[2] = {64, 350};
constexpr int32_t kEarY = 92;
constexpr int32_t kEarSize = 52;
constexpr int32_t kTailX[3] = {374, 392, 406};
constexpr int32_t kTailY[3] = {306, 282, 254};
constexpr int32_t kTailSize[3] = {18, 22, 26};
constexpr uint8_t kSilhouetteOpa = 200;

// 困倦 / 入睡时的眼睛高度与瞳孔下垂量。
constexpr int32_t kEyeSleepyH = 86;
constexpr int32_t kEyeSleepingH = 46;
constexpr int32_t kSleepyIrisDrop = 8;

// 腮红：默认 12%，开心 30%。
constexpr uint8_t kBlushOpa = 31;
constexpr uint8_t kBlushHappyOpa = 77;

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
constexpr uint32_t kHappyMs = 600;
// 呼吸：身体整体 +12 → -12px（24px 行程），3s 单程（6s 周期）；
// 困倦 4.5s 单程（9s 周期）。根容器黑底足够大，移动不会露出仪表。
constexpr int32_t kBreathOffsetPx = 12;
constexpr uint32_t kBreathHalfMs = 3000;
constexpr uint32_t kBreathSleepyHalfMs = 4500;

constexpr int32_t kGazeRangeX = 22;
constexpr int32_t kGazeRangeY = 14;

// "z z z" 三个字母的位置（挂在根容器上）。
constexpr int32_t kZX[3] = {300, 330, 356};
constexpr int32_t kZY[3] = {118, 90, 62};

int32_t randomBetween(int32_t low, int32_t high) {
  if (high <= low) {
    return low;
  }
  return low + static_cast<int32_t>(
                   esp_random() % static_cast<uint32_t>(high - low + 1));
}

bool due(uint32_t at) { return static_cast<int32_t>(millis() - at) >= 0; }

int32_t clampInt(int32_t value, int32_t low, int32_t high) {
  if (value < low) {
    return low;
  }
  if (value > high) {
    return high;
  }
  return value;
}

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

lv_obj_t* makeText(lv_obj_t* parent, int32_t x, int32_t y, int32_t w,
                   int32_t h, const char* text, lv_color_t color) {
  lv_obj_t* label = lv_label_create(parent);
  lv_label_set_text(label, text);
  lv_obj_set_pos(label, x, y);
  lv_obj_set_size(label, w, h);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(label, color, 0);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_opa(label, LV_OPA_COVER, 0);
  lv_obj_set_style_opa(label, 0, 0);
  lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(label, 0, 0);
  lv_obj_set_style_pad_all(label, 0, 0);
  lv_obj_set_clickable(label, false);
  return label;
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
  m_pupilBaseSize = kPupilSize;
  m_timer = lv_timer_create(timerCallback, 50, this);
  m_built = true;
  CFG_LOG("[ELF] overlay built, heap %lu",
          static_cast<unsigned long>(ESP.getFreeHeap()));
}

void ElfOverlay::buildObjects() {
  lv_obj_t* screen = lv_screen_active();

  // 根容器：不透明纯黑（AMOLED 关闭像素），盖住下面的仪表页；
  // 不可点击、默认隐藏。
  m_root = makeBlock(screen, 0, 0, kScreenSize, kScreenSize, 0, kElfBg,
                     LV_OPA_COVER);
  lv_obj_set_hidden(m_root, true);

  // 装饰弧：R210、1px、8% 青色，用大圆边框代替阴影。
  m_aura = makeBlock(m_root, 23, 23, 420, 420, LV_RADIUS_CIRCLE,
                     lv_color_black(), LV_OPA_TRANSP);
  lv_obj_set_style_border_width(m_aura, 1, 0);
  lv_obj_set_style_border_color(m_aura, kGlow, 0);
  lv_obj_set_style_border_opa(m_aura, 20, 0);  // 约 8%

  // 身体容器：眼睛/腮红/嘴/zzz 都挂在这里，呼吸时整体上下漂移，
  // 根容器的黑底和装饰弧保持不动，画面边缘不会露出仪表。
  m_body = makeBlock(m_root, 0, 0, kScreenSize, kScreenSize, 0,
                     lv_color_black(), LV_OPA_TRANSP);

  // 阶段 4：低对比度耳朵 + 尾巴剪影，放在眼睛之前，只做轮廓。
  for (int i = 0; i < 2; ++i) {
    m_ear[i] = makeBlock(m_body, kEarX[i], kEarY, kEarSize, kEarSize,
                         LV_RADIUS_CIRCLE, kSilhouette, kSilhouetteOpa);
  }
  for (int i = 0; i < 3; ++i) {
    m_tail[i] = makeBlock(m_body, kTailX[i], kTailY[i], kTailSize[i],
                          kTailSize[i], LV_RADIUS_CIRCLE, kSilhouette,
                          kSilhouetteOpa);
  }

  for (int i = 0; i < 2; ++i) {
    // 眼球：胶囊形，子对象随高度压缩被裁掉，形成“合眼”。
    m_eye[i] = makeBlock(m_body, kEyeX[i], kEyeY, kEyeW, kEyeH, 64, kSclera,
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
    m_blush[i] = makeBlock(m_body, kBlushX[i], 298, 34, 16, 8, kBlush,
                           kBlushOpa);
  }

  // 嘴：常态可见；开心/打哈欠时提高不透明度。
  m_mouth = makeBlock(m_body, 211, 342, 44, 14, 7, kMouthColor,
                      kMouthIdleOpa);

  // 入睡时出现的 ASCII "z z z"。
  for (int i = 0; i < 3; ++i) {
    m_z[i] = makeText(m_body, kZX[i], kZY[i], 36, 34, "z", kSleep);
    lv_obj_set_hidden(m_z[i], true);
  }

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
      m_state == State::Blinking || m_state == State::Happy ||
      m_state == State::Sleepy || m_state == State::Sleeping) {
    return;
  }
  // 从 Hidden 或 Hiding 进入：清掉残留动画，从当前高度睁眼。
  lv_anim_delete(this, setEyeOpen);
  lv_anim_delete(this, setGazeX);
  lv_anim_delete(this, setGazeY);
  stopZzz();
  resetHappyVisuals();
  lv_obj_set_hidden(m_root, false);
  lv_obj_move_foreground(m_root);
  m_state = State::Waking;
  m_blinkPhase = BlinkPhase::None;
  m_blinkIsSecond = false;
  m_yawnHolding = false;
  m_squintUntil = 0;
  m_lastInteractionAt = millis();
  // 呼出时重新排一次哈欠，避免用掉“上次隐藏前”的过期时间立即打哈欠。
  if (m_charging) {
    m_nextYawnAt = m_lastInteractionAt +
                   randomBetween(kYawnFirstMinMs, kYawnFirstMaxMs);
  }

  int32_t currentH = lv_obj_get_height(m_eye[0]);
  if (currentH < kEyeClosedH) {
    currentH = kEyeClosedH;
  }
  setEyeOpen(this, currentH);
  startBreathing(kBreathHalfMs);
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
  stopZzz();
  resetHappyVisuals();
  m_state = State::Hiding;
  m_blinkPhase = BlinkPhase::None;
  m_yawnHolding = false;
  m_squintUntil = 0;

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

void ElfOverlay::startBreathing(uint32_t halfPeriodMs) {
  lv_anim_delete(m_body, setBodyY);
  lv_obj_set_y(m_body, kBreathOffsetPx);
  lv_anim_t anim;
  lv_anim_init(&anim);
  lv_anim_set_var(&anim, m_body);
  lv_anim_set_exec_cb(&anim, setBodyY);
  lv_anim_set_values(&anim, kBreathOffsetPx, -kBreathOffsetPx);
  lv_anim_set_duration(&anim, halfPeriodMs);
  lv_anim_set_reverse_duration(&anim, halfPeriodMs);
  lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
  lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
  lv_anim_start(&anim);
}

void ElfOverlay::stopBreathing() {
  lv_anim_delete(m_body, setBodyY);
  lv_obj_set_y(m_body, 0);
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

void ElfOverlay::lookAt(int16_t screenX, int16_t screenY) {
  if (!m_built || m_state == State::Hidden || m_state == State::Waking ||
      m_state == State::Hiding) {
    return;
  }
  if (m_state == State::Sleepy || m_state == State::Sleeping) {
    wakeFromSleep();
  }
  notifyInteraction();

  // 手指位置换算成虹膜偏移：屏幕中心 → 0，边缘 → 最大偏移。
  const int32_t dx = static_cast<int32_t>(screenX) - kScreenSize / 2;
  const int32_t dy = static_cast<int32_t>(screenY) - kEyeCenterY;
  const int32_t offsetX =
      clampInt(dx * kGazeRangeX / (kScreenSize / 2), -kGazeRangeX, kGazeRangeX);
  const int32_t offsetY =
      clampInt(dy * kGazeRangeY / kEyeCenterY, -kGazeRangeY, kGazeRangeY);

  // 追随是直接改位置，不做动画；先清掉随机注视的动画，避免互相抢。
  lv_anim_delete(this, setGazeX);
  lv_anim_delete(this, setGazeY);
  setGazeX(this, kIrisInset + offsetX);
  setGazeY(this, kIrisY + offsetY);
}

void ElfOverlay::touchEnd() {
  if (!m_built) {
    return;
  }
  m_lastInteractionAt = millis();
  // 松手 600ms 后回到随机游走；Happy 自己会在结束时重新排期。
  if (m_state == State::Idle) {
    m_nextGazeAt = m_lastInteractionAt + 600;
  }
}

void ElfOverlay::poke(int16_t screenX, int16_t screenY) {
  if (!m_built || m_state == State::Hidden || m_state == State::Waking ||
      m_state == State::Hiding) {
    return;
  }
  if (m_state == State::Sleepy || m_state == State::Sleeping) {
    wakeFromSleep();
  }
  lookAt(screenX, screenY);
  notifyInteraction();
  startHappy();
}

void ElfOverlay::notifyInteraction() {
  m_lastInteractionAt = millis();
  if (m_state == State::Sleepy || m_state == State::Sleeping) {
    wakeFromSleep();
  }
}

void ElfOverlay::setVehicleState(float speedKmh, bool speedValid,
                                 bool charging) {
  if (!m_built) {
    return;
  }
  const uint32_t now = millis();

  // 充电上升沿：几秒后第一次打哈欠；拔出充电则取消排期。
  if (charging != m_charging) {
    m_charging = charging;
    m_nextYawnAt =
        charging ? now + randomBetween(kYawnFirstMinMs, kYawnFirstMaxMs) : 0;
    CFG_LOG("[ELF] charging %s", charging ? "on" : "off");
  }

  // 车速瞳孔：20 km/h 起随速度平滑放大，100 km/h 到 86。
  int32_t target = kPupilSize;
  if (speedValid) {
    float t = (speedKmh - kPupilSpeedStartKmh) /
              (kPupilSpeedFullKmh - kPupilSpeedStartKmh);
    if (t < 0.0f) {
      t = 0.0f;
    } else if (t > 1.0f) {
      t = 1.0f;
    }
    target = kPupilSize + static_cast<int32_t>(
                              t * (kPupilSpeedMaxSize - kPupilSize) + 0.5f);
  }
  if (target != m_pupilBaseSize) {
    const int32_t fromSize = lv_obj_get_width(m_pupil[0]);
    m_pupilBaseSize = target;
    lv_anim_delete(this, setPupilSize);
    if (m_state == State::Hidden) {
      setPupilSize(this, target);
    } else if (m_state != State::Hiding && m_state != State::Happy) {
      // Happy 期间不抢瞳孔动画，结束时自然回到新的基准值。
      startAnim(this, setPupilSize, fromSize, target, kPupilAnimMs,
                lv_anim_path_ease_out, nullptr);
    }
    CFG_LOG("[ELF] pupil %d (speed %.0f)", static_cast<int>(target),
            static_cast<double>(speedKmh));
  }

  // 行驶中保持清醒：车速只要超过阈值就视作持续交互，并唤醒已睡着的精灵。
  if (speedValid && speedKmh >= kDrivingSpeedKmh) {
    const bool wasAsleep =
        m_state == State::Sleepy || m_state == State::Sleeping;
    m_lastInteractionAt = now;
    if (wasAsleep) {
      wakeFromSleep();
    }
  }

  // 急减速：一次采样内降幅 ≥ 10 km/h，且当前空闲 → 眯眼 600ms。
  if (speedValid) {
    if (m_haveSpeedSample) {
      const uint32_t dt = now - m_lastSpeedAt;
      const float drop = m_lastSpeedKmh - speedKmh;
      if (dt > 0 && dt <= kDecelWindowMs && drop >= kDecelDropKmh &&
          m_state == State::Idle && due(m_squintCooldownUntil)) {
        startSquint();
      }
    }
    m_lastSpeedKmh = speedKmh;
    m_lastSpeedAt = now;
    m_haveSpeedSample = true;
  } else {
    m_haveSpeedSample = false;
  }
}

void ElfOverlay::enterSleepy() {
  if (m_state != State::Idle) {
    return;
  }
  m_state = State::Sleepy;
  m_blinkPhase = BlinkPhase::None;
  lv_anim_delete(this, setEyeOpen);
  lv_anim_delete(this, setGazeX);
  lv_anim_delete(this, setGazeY);

  const int32_t currentH = lv_obj_get_height(m_eye[0]);
  animateEyes(currentH, kEyeSleepyH, 400, lv_anim_path_ease_in_out, nullptr);
  startAnim(this, setGazeY, lv_obj_get_y(m_iris[0]),
            kIrisY + kSleepyIrisDrop, 500, lv_anim_path_ease_out, nullptr);
  startBreathing(kBreathSleepyHalfMs);
  CFG_LOG("[ELF] sleepy");
}

void ElfOverlay::enterSleeping() {
  if (m_state != State::Sleepy) {
    return;
  }
  m_state = State::Sleeping;
  lv_anim_delete(this, setEyeOpen);
  const int32_t currentH = lv_obj_get_height(m_eye[0]);
  animateEyes(currentH, kEyeSleepingH, 500, lv_anim_path_ease_in_out, nullptr);
  startZzz();
  CFG_LOG("[ELF] sleeping");
}

void ElfOverlay::wakeFromSleep() {
  if (m_state != State::Sleepy && m_state != State::Sleeping) {
    return;
  }
  const bool wasSleeping = m_state == State::Sleeping;
  m_state = State::Idle;
  m_blinkPhase = BlinkPhase::None;
  lv_anim_delete(this, setEyeOpen);
  lv_anim_delete(this, setGazeX);
  lv_anim_delete(this, setGazeY);
  stopZzz();

  const int32_t currentH = lv_obj_get_height(m_eye[0]);
  animateEyes(currentH, kEyeH, 300, lv_anim_path_ease_out, nullptr);
  startAnim(this, setGazeY, lv_obj_get_y(m_iris[0]), kIrisY, 300,
            lv_anim_path_ease_out, nullptr);
  startBreathing(kBreathHalfMs);

  const uint32_t now = millis();
  m_nextBlinkAt =
      now + randomBetween(CFG_ELF_BLINK_MIN_MS, CFG_ELF_BLINK_MAX_MS);
  m_nextGazeAt =
      now + randomBetween(CFG_ELF_GAZE_MIN_MS, CFG_ELF_GAZE_MAX_MS);
  CFG_LOG("[ELF] wake from %s", wasSleeping ? "sleep" : "sleepy");
}

void ElfOverlay::startHappy() {
  if (m_state == State::Hidden || m_state == State::Waking ||
      m_state == State::Hiding) {
    return;
  }
  lv_anim_delete(this, setEyeOpen);
  lv_anim_delete(this, setPupilSize);
  lv_anim_delete(this, setBlushOpa);
  stopZzz();
  m_state = State::Happy;
  m_blinkPhase = BlinkPhase::None;
  m_happyUntil = millis() + kHappyMs;
  lv_anim_delete(this, setMouthOpa);
  startAnim(this, setMouthOpa, lv_obj_get_style_bg_opa(m_mouth, LV_PART_MAIN),
            kMouthHappyOpa, 200, lv_anim_path_ease_out, nullptr);

  // 快速眨两下：150 → 10（90ms），10 → 150（140ms），重复 2 次。
  setEyeOpen(this, kEyeH);  // 先回到全开，保证双眨完整
  lv_anim_t blink;
  lv_anim_init(&blink);
  lv_anim_set_var(&blink, this);
  lv_anim_set_exec_cb(&blink, setEyeOpen);
  lv_anim_set_values(&blink, kEyeH, kEyeClosedH);
  lv_anim_set_duration(&blink, 90);
  lv_anim_set_reverse_duration(&blink, 140);
  lv_anim_set_repeat_count(&blink, 2);
  lv_anim_set_path_cb(&blink, lv_anim_path_ease_in);
  lv_anim_start(&blink);

  startAnim(this, setPupilSize, lv_obj_get_width(m_pupil[0]), kPupilHappySize,
            200, lv_anim_path_ease_out, nullptr);
  startAnim(this, setBlushOpa, kBlushOpa, kBlushHappyOpa, 200,
            lv_anim_path_ease_out, nullptr);
  CFG_LOG("[ELF] happy");
}

void ElfOverlay::endHappy() {
  if (m_state != State::Happy) {
    return;
  }
  startAnim(this, setPupilSize, kPupilHappySize, m_pupilBaseSize, 200,
            lv_anim_path_ease_out, nullptr);
  startAnim(this, setBlushOpa, kBlushHappyOpa, kBlushOpa, 300,
            lv_anim_path_ease_out, nullptr);
  lv_anim_delete(this, setMouthOpa);
  startAnim(this, setMouthOpa, kMouthHappyOpa, kMouthIdleOpa, 300,
            lv_anim_path_ease_out, nullptr);
  m_state = State::Idle;

  const uint32_t now = millis();
  m_nextBlinkAt =
      now + randomBetween(CFG_ELF_BLINK_MIN_MS, CFG_ELF_BLINK_MAX_MS);
  // 被摸后先看手指停留约 300ms，再回到随机游走。
  m_nextGazeAt = now + 300;
  CFG_LOG("[ELF] happy end");
}

void ElfOverlay::resetHappyVisuals() {
  lv_anim_delete(this, setPupilSize);
  lv_anim_delete(this, setBlushOpa);
  lv_anim_delete(this, setMouthOpa);
  setPupilSize(this, m_pupilBaseSize);
  setBlushOpa(this, kBlushOpa);
  setMouthOpa(this, kMouthIdleOpa);
}

void ElfOverlay::startYawn() {
  if (m_state != State::Idle) {
    return;
  }
  lv_anim_delete(this, setEyeOpen);
  lv_anim_delete(this, setGazeX);
  lv_anim_delete(this, setGazeY);
  lv_anim_delete(this, setMouthOpa);
  setMouthOpa(this, kMouthYawnOpa);
  m_state = State::Yawning;
  m_blinkPhase = BlinkPhase::None;
  m_yawnHolding = false;

  const int32_t currentH = lv_obj_get_height(m_eye[0]);
  animateEyes(currentH, kEyeClosedH, kYawnCloseMs, lv_anim_path_ease_in_out,
              onYawnClosed);
  CFG_LOG("[ELF] yawn");
}

void ElfOverlay::startSquint() {
  if (m_state != State::Idle) {
    return;
  }
  lv_anim_delete(this, setEyeOpen);
  m_state = State::Squint;
  m_blinkPhase = BlinkPhase::None;
  m_squintUntil = millis() + kSquintHoldMs;
  m_squintCooldownUntil = millis() + kSquintCooldownMs;

  const int32_t currentH = lv_obj_get_height(m_eye[0]);
  animateEyes(currentH, kEyeSquintH, kSquintInMs, lv_anim_path_ease_out,
              nullptr);
  CFG_LOG("[ELF] squint");
}

void ElfOverlay::startZzz() {
  stopZzz();
  for (int i = 0; i < 3; ++i) {
    lv_obj_set_hidden(m_z[i], false);
    lv_obj_set_style_opa(m_z[i], 180, 0);
  }
  lv_anim_t anim;
  lv_anim_init(&anim);
  lv_anim_set_var(&anim, this);
  lv_anim_set_exec_cb(&anim, setZOpacity);
  lv_anim_set_values(&anim, 0, 1000);
  lv_anim_set_duration(&anim, 1200);
  lv_anim_set_path_cb(&anim, lv_anim_path_linear);
  lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
  lv_anim_start(&anim);
  CFG_LOG("[ELF] zzz on");
}

void ElfOverlay::stopZzz() {
  lv_anim_delete(this, setZOpacity);
  for (int i = 0; i < 3; ++i) {
    if (m_z[i] != nullptr) {
      lv_obj_set_hidden(m_z[i], true);
    }
  }
}

void ElfOverlay::onTimer() {
  if (!m_built) {
    return;
  }
  const uint32_t now = millis();

  if (m_state == State::Happy) {
    if (due(m_happyUntil)) {
      endHappy();
    }
    return;
  }

  // 打哈欠：慢闭 → 停 400ms → 慢睁。
  if (m_state == State::Yawning) {
    if (m_yawnHolding && now - m_yawnClosedAt >= kYawnHoldMs) {
      m_yawnHolding = false;
      animateEyes(kEyeClosedH, kEyeH, kYawnOpenMs, lv_anim_path_ease_out,
                  onYawnOpened);
    }
    return;
  }

  // 急减速眯眼：眯 600ms 后睁开。
  if (m_state == State::Squint) {
    if (m_squintUntil != 0 && due(m_squintUntil)) {
      m_squintUntil = 0;
      animateEyes(lv_obj_get_height(m_eye[0]), kEyeH, kSquintOutMs,
                  lv_anim_path_ease_out, onSquintDone);
    }
    return;
  }

  // 眨眼“停 40ms”阶段结束后睁眼。
  if (m_state == State::Blinking && m_blinkPhase == BlinkPhase::Holding &&
      now - m_blinkClosedAt >= kBlinkHoldMs) {
    m_blinkPhase = BlinkPhase::Opening;
    animateEyes(kEyeClosedH, kEyeH, kBlinkOpenMs, lv_anim_path_ease_out,
                onBlinkOpened);
    return;
  }

#if CFG_ELF_AUTO_HIDE_MS > 0
  // 自动隐藏默认关闭（CFG_ELF_AUTO_HIDE_MS = 0）。
  if (m_state == State::Idle || m_state == State::Sleepy ||
      m_state == State::Sleeping) {
    if (due(m_lastInteractionAt + CFG_ELF_AUTO_HIDE_MS)) {
      CFG_LOG("[ELF] auto hide");
      hide();
      return;
    }
  }
#endif

  if (m_state == State::Idle) {
    if (due(m_lastInteractionAt + CFG_ELF_SLEEP_MS)) {
      enterSleepy();
    } else if (m_charging && m_nextYawnAt != 0 && due(m_nextYawnAt)) {
      startYawn();
    } else if (due(m_nextBlinkAt)) {
      startBlink();
    } else if (due(m_nextGazeAt)) {
      startGaze();
    }
    return;
  }

  if (m_state == State::Sleepy &&
      due(m_lastInteractionAt + CFG_ELF_SLEEP_DEEP_MS)) {
    enterSleeping();
  }
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

void ElfOverlay::setBodyY(void* var, int32_t y) {
  lv_obj_set_y(static_cast<lv_obj_t*>(var), y);
}

void ElfOverlay::setPupilSize(void* var, int32_t size) {
  auto* self = static_cast<ElfOverlay*>(var);
  if (self == nullptr) {
    return;
  }
  const int32_t inset = (kIrisSize - size) / 2;
  for (int i = 0; i < 2; ++i) {
    lv_obj_set_pos(self->m_pupil[i], inset, inset);
    lv_obj_set_size(self->m_pupil[i], size, size);
  }
}

void ElfOverlay::setBlushOpa(void* var, int32_t opacity) {
  auto* self = static_cast<ElfOverlay*>(var);
  if (self == nullptr) {
    return;
  }
  for (int i = 0; i < 2; ++i) {
    lv_obj_set_style_bg_opa(self->m_blush[i],
                            static_cast<lv_opa_t>(opacity), 0);
  }
}

void ElfOverlay::setMouthOpa(void* var, int32_t opacity) {
  auto* self = static_cast<ElfOverlay*>(var);
  if (self == nullptr || self->m_mouth == nullptr) {
    return;
  }
  lv_obj_set_style_bg_opa(self->m_mouth, static_cast<lv_opa_t>(opacity), 0);
}

void ElfOverlay::setZOpacity(void* var, int32_t progress) {
  auto* self = static_cast<ElfOverlay*>(var);
  if (self == nullptr) {
    return;
  }
  for (int i = 0; i < 3; ++i) {
    // 三个 z 相位错开，形成依次亮起的效果。
    const int32_t phase = (progress + i * 333) % 1000;
    const int32_t wave = phase < 500 ? phase : 1000 - phase;
    const int32_t opacity = 70 + wave * 185 / 500;
    lv_obj_set_style_opa(self->m_z[i], static_cast<lv_opa_t>(opacity), 0);
  }
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
  lv_obj_set_hidden(self->m_root, true);
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

void ElfOverlay::onYawnClosed(lv_anim_t* anim) {
  auto* self = static_cast<ElfOverlay*>(anim->var);
  if (self == nullptr || self->m_state != State::Yawning) {
    return;
  }
  self->m_yawnClosedAt = millis();
  self->m_yawnHolding = true;
}

void ElfOverlay::onYawnOpened(lv_anim_t* anim) {
  auto* self = static_cast<ElfOverlay*>(anim->var);
  if (self == nullptr || self->m_state != State::Yawning) {
    return;
  }
  self->m_state = State::Idle;
  self->m_yawnHolding = false;
  startAnim(self, setMouthOpa, kMouthYawnOpa, kMouthIdleOpa, 300,
            lv_anim_path_ease_out, nullptr);

  const uint32_t now = millis();
  self->m_nextYawnAt =
      self->m_charging ? now + randomBetween(kYawnNextMinMs, kYawnNextMaxMs) : 0;
  self->m_nextBlinkAt =
      now + randomBetween(CFG_ELF_BLINK_MIN_MS, CFG_ELF_BLINK_MAX_MS);
  self->m_nextGazeAt =
      now + randomBetween(CFG_ELF_GAZE_MIN_MS, CFG_ELF_GAZE_MAX_MS);
  CFG_LOG("[ELF] yawn end");
}

void ElfOverlay::onSquintDone(lv_anim_t* anim) {
  auto* self = static_cast<ElfOverlay*>(anim->var);
  if (self == nullptr || self->m_state != State::Squint) {
    return;
  }
  self->m_state = State::Idle;
  const uint32_t now = millis();
  self->m_nextBlinkAt =
      now + randomBetween(CFG_ELF_BLINK_MIN_MS, CFG_ELF_BLINK_MAX_MS);
  self->m_nextGazeAt =
      now + randomBetween(CFG_ELF_GAZE_MIN_MS, CFG_ELF_GAZE_MAX_MS);
  CFG_LOG("[ELF] squint end");
}

#else  // CFG_ELF_ENABLE == 0

void ElfOverlay::begin() {}
bool ElfOverlay::visible() const { return false; }
void ElfOverlay::show() {}
void ElfOverlay::hide() {}
void ElfOverlay::lookAt(int16_t, int16_t) {}
void ElfOverlay::touchEnd() {}
void ElfOverlay::poke(int16_t, int16_t) {}
void ElfOverlay::setVehicleState(float, bool, bool) {}

#endif  // CFG_ELF_ENABLE
