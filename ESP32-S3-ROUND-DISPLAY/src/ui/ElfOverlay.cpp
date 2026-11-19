#include "ui/ElfOverlay.h"

#include <Arduino.h>
#include <esp_system.h>
#include <math.h>

#include "config.h"

// 声音配置由并行改动写入 config.h（design/ELF-DESIGN.md §5.6）。
// 这里只做缺省兜底，避免引用到未定义的宏；已定义时不会覆盖。
#ifndef CFG_ELF_SOUND_ENABLE
#define CFG_ELF_SOUND_ENABLE 0
#endif
#ifndef CFG_ELF_SOUND_NOISE_GATE
#define CFG_ELF_SOUND_NOISE_GATE 8
#endif
#ifndef CFG_ELF_SOUND_QUIET_MS
#define CFG_ELF_SOUND_QUIET_MS 2000UL
#endif
#ifndef CFG_ELF_SOUND_KEEP_AWAKE
#define CFG_ELF_SOUND_KEEP_AWAKE 1
#endif
#ifndef CFG_ELF_SOUND_BEAT_MIN_BPM
#define CFG_ELF_SOUND_BEAT_MIN_BPM 60
#endif
#ifndef CFG_ELF_SOUND_BEAT_MAX_BPM
#define CFG_ELF_SOUND_BEAT_MAX_BPM 200
#endif
#ifndef CFG_ELF_SOUND_BEAT_CONFIRM
#define CFG_ELF_SOUND_BEAT_CONFIRM 2
#endif
#ifndef CFG_ELF_SOUND_GROOVE_HOLD_MS
#define CFG_ELF_SOUND_GROOVE_HOLD_MS 1600UL
#endif
#ifndef CFG_ELF_SOUND_BOUNCE_PX
#define CFG_ELF_SOUND_BOUNCE_PX 20
#endif
#ifndef CFG_ELF_SOUND_SWAY_PX
#define CFG_ELF_SOUND_SWAY_PX 12
#endif

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
// 呼吸：身体整体 +22 → -22px（44px 行程），3s 单程（6s 周期）；
// 困倦 4.5s 单程（9s 周期）。根容器黑底足够大，移动不会露出仪表。
constexpr int32_t kBreathOffsetPx = 22;
constexpr uint32_t kBreathHalfMs = 3000;
constexpr uint32_t kBreathSleepyHalfMs = 4500;

// 视线范围：左右 ±30、上下 ±22（虹膜会被眼球边缘自然裁切，形成“瞟”的感觉）。
// 左右注视时身体额外平移 ±kGazeLeanX，放大整体位置变化。
constexpr int32_t kGazeRangeX = 30;
constexpr int32_t kGazeRangeY = 22;
constexpr int32_t kGazeLeanX = 10;

// "z z z" 三个字母的位置（挂在根容器上）。
constexpr int32_t kZX[3] = {300, 330, 356};
constexpr int32_t kZY[3] = {118, 90, 62};

// 嘴 / 光环的基准几何（阶段 5 张嘴动画需要中心不变）。
constexpr int32_t kMouthW = 44;
constexpr int32_t kMouthH = 14;
constexpr int32_t kMouthCenterY = 342 + kMouthH / 2;  // 349
constexpr int32_t kAuraBaseOpa = 20;                   // 约 8%

// 阶段 5：声音律动（见 design/ELF-DESIGN.md §4.3）。
constexpr uint32_t kSoundTimerMs = 30;
constexpr uint32_t kPulseDecayMs = 120;    // pulse 按 exp(-t/120ms) 衰减
constexpr uint32_t kBeatWindowMs = 1200;   // 进入 Grooving 的拍点统计窗口
constexpr uint32_t kGrooveFadeInMs = 150;  // 进入 Grooving 的偏移升入时间
constexpr uint32_t kGrooveFadeOutMs = 500; // 退出 Grooving 的平滑归零
constexpr uint32_t kGrooveBoostMs = 180;   // 高潮拍：g 临时 ×1.3 的持续
constexpr uint32_t kSoundYieldMs = 600;    // 触摸后声音层让位时间
constexpr uint32_t kMusicBubbleDurationMs = 1200;
constexpr int32_t kMusicBubbleRisePx = 84;
constexpr int32_t kPupilSoundMaxSize = 90; // 声音/车速/Happy 叠加后的瞳孔上限
constexpr int32_t kMouthMaxH = 24;
constexpr int32_t kSoundGazeX = 24;        // Grooving 拍点换向的视线摆幅
constexpr uint8_t kBlushSoundMaxOpa = 77;  // 和 Happy 上限一致，避免抢
constexpr uint8_t kAuraSoundMaxOpa = 77;   // 上限 30%
constexpr uint32_t kAuraAmbientPeriodMs = 3200;
constexpr int32_t kAuraAmbientPulseOpa = 8;
constexpr uint8_t kMusicBubblesPerBeat = 4;
constexpr int32_t kMusicBubbleMinSize = 14;
constexpr int32_t kMusicBubbleMaxSize = 30;
constexpr int32_t kMusicBubbleGrowthPx = 8;

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
#if CFG_ELF_SOUND_ENABLE
  // 声音层独立 30ms 定时器：不改变既有 50ms 状态机节奏。
  m_soundLastTick = millis();
  m_soundTimer = lv_timer_create(soundTimerCallback, kSoundTimerMs, this);
  resetSoundState();
#endif
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

#if CFG_ELF_SOUND_ENABLE
  for (int i = 0; i < kMusicBubbleCount; ++i) {
    m_bubble[i] = makeBlock(m_body, 0, 0, kMusicBubbleMinSize,
                            kMusicBubbleMinSize, LV_RADIUS_CIRCLE, kGlow, 72);
    lv_obj_set_style_border_width(m_bubble[i], 2, 0);
    lv_obj_set_style_border_color(m_bubble[i], lv_color_white(), 0);
    lv_obj_set_style_border_opa(m_bubble[i], 180, 0);
    lv_obj_set_hidden(m_bubble[i], true);
  }
#endif

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

void ElfOverlay::emitMusicBubble(uint32_t now) {
#if CFG_ELF_SOUND_ENABLE
  const uint16_t beatHue =
      static_cast<uint16_t>((185 + (m_beatInGroove % 8) * 45) % 360);
  for (int i = 0; i < kMusicBubblesPerBeat; ++i) {
    const uint8_t index = m_nextBubble;
    m_nextBubble = static_cast<uint8_t>((m_nextBubble + 1) % kMusicBubbleCount);
    m_bubbleStartedAt[index] = now;
    const bool leftSide = (randomBetween(0, 1) == 0);
    m_bubbleStartX[index] = static_cast<int16_t>(
        leftSide ? randomBetween(18, 82) : randomBetween(384, 448));
    m_bubbleStartY[index] =
        static_cast<int16_t>(randomBetween(170, 315));
    m_bubbleBaseSize[index] = static_cast<uint8_t>(
        randomBetween(kMusicBubbleMinSize, kMusicBubbleMaxSize));
    m_bubbleHue[index] = static_cast<uint16_t>(
        (beatHue + i * 42 + randomBetween(0, 24)) % 360);
    m_bubbleActive[index] = true;
    lv_obj_set_style_bg_color(
        m_bubble[index],
        lv_color_hsv_to_rgb(m_bubbleHue[index], 190, 245), 0);
    lv_obj_set_style_border_color(
        m_bubble[index],
        lv_color_hsv_to_rgb(m_bubbleHue[index], 75, 255), 0);
    lv_obj_set_pos(m_bubble[index],
                   m_bubbleStartX[index] - m_bubbleBaseSize[index] / 2,
                   m_bubbleStartY[index] - m_bubbleBaseSize[index] / 2);
    lv_obj_set_size(m_bubble[index], m_bubbleBaseSize[index],
                    m_bubbleBaseSize[index]);
    lv_obj_set_style_bg_opa(m_bubble[index], 72, 0);
    lv_obj_set_style_border_opa(m_bubble[index], 180, 0);
    lv_obj_set_hidden(m_bubble[index], false);
  }
#endif
}

void ElfOverlay::updateMusicBubbles(uint32_t now) {
#if CFG_ELF_SOUND_ENABLE
  for (int i = 0; i < kMusicBubbleCount; ++i) {
    if (!m_bubbleActive[i]) {
      continue;
    }

    const uint32_t age = now - m_bubbleStartedAt[i];
    if (age >= kMusicBubbleDurationMs || m_state == State::Hidden ||
        m_state == State::Hiding) {
      m_bubbleActive[i] = false;
      lv_obj_set_hidden(m_bubble[i], true);
      continue;
    }

    const float progress =
        static_cast<float>(age) / kMusicBubbleDurationMs;
    const float rise = progress * (2.0f - progress);
    const float drift =
        sinf(progress * 6.2831853f + i * 0.7f) * 12.0f;
    const int32_t size =
        m_bubbleBaseSize[i] +
        static_cast<int32_t>(kMusicBubbleGrowthPx * rise + 0.5f);
    const int32_t x = m_bubbleStartX[i] +
                      static_cast<int32_t>(lroundf(drift)) - size / 2;
    const int32_t y = m_bubbleStartY[i] -
                      static_cast<int32_t>(kMusicBubbleRisePx * rise) -
                      size / 2;
    const float fade = 1.0f - progress;
    lv_obj_set_pos(m_bubble[i], x, y);
    lv_obj_set_size(m_bubble[i], size, size);
    lv_obj_set_style_bg_opa(
        m_bubble[i], static_cast<lv_opa_t>(72.0f * fade + 0.5f), 0);
    lv_obj_set_style_border_opa(
        m_bubble[i], static_cast<lv_opa_t>(180.0f * fade + 0.5f), 0);
  }
#else
  (void)now;
#endif
}

void ElfOverlay::hideMusicBubbles() {
  for (int i = 0; i < kMusicBubbleCount; ++i) {
    m_bubbleActive[i] = false;
    if (m_bubble[i] != nullptr) {
      lv_obj_set_hidden(m_bubble[i], true);
    }
  }
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
  lv_anim_delete(m_body, setBodyX);
  stopZzz();
  resetHappyVisuals();
#if CFG_ELF_SOUND_ENABLE
  // 重新呼出：声音子状态从 Off 开始，等第一帧再进 Quiet。
  resetSoundState();
  resetSoundVisuals(false);
#endif
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
  lv_obj_set_x(m_body, 0);
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
#if CFG_ELF_SOUND_ENABLE
  // 隐藏时立即复位声音偏移，并让子状态回到 Off。
  resetSoundVisuals(false);
  resetSoundState();
#endif
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
  lv_anim_delete(this, setBodyYOnSelf);
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
  lv_anim_delete(this, setBodyYOnSelf);
  lv_obj_set_y(m_body, 0);
}

void ElfOverlay::setBodyYOnSelf(void* var, int32_t y) {
  auto* self = static_cast<ElfOverlay*>(var);
  if (self != nullptr) {
    lv_obj_set_y(self->m_body, y);
  }
}

#if CFG_ELF_SOUND_ENABLE
// 声音层退出 / 主状态接管时，从当前的律动 y 位置平滑回到呼吸起点。
// 否则 startBreathing() 会把 m_body.y 直接设成 +22，产生一次可见跳变。
void ElfOverlay::resumeBreathing() {
  const int32_t from = lv_obj_get_y(m_body);
  if (from == kBreathOffsetPx) {
    startBreathing(kBreathHalfMs);
    return;
  }
  lv_anim_delete(m_body, setBodyY);
  lv_anim_delete(this, setBodyYOnSelf);
  startAnim(this, setBodyYOnSelf, from, kBreathOffsetPx, 350,
            lv_anim_path_ease_out, onResumeBreathDone);
}

void ElfOverlay::onResumeBreathDone(lv_anim_t* anim) {
  auto* self = static_cast<ElfOverlay*>(anim->var);
  if (self == nullptr || self->m_soundVisApplied ||
      self->m_soundBreathStopped) {
    return;
  }
  // Happy/Yawning/Squint 也保持呼吸；Sleepy/Sleeping 会用自己的周期并
  // 在此之前删除本动画，所以这里只需避开隐藏态。
  if (self->m_state == State::Hidden || self->m_state == State::Hiding) {
    return;
  }
  self->startBreathing(kBreathHalfMs);
}
#endif  // CFG_ELF_SOUND_ENABLE

void ElfOverlay::startBlink() {
  if (m_state != State::Idle) {
    return;
  }
  m_state = State::Blinking;
  m_blinkPhase = BlinkPhase::Closing;
  // 从当前高度开始：Grooving 的“开心眼”可能把眼睛撑高了几像素。
  int32_t currentH = lv_obj_get_height(m_eye[0]);
  if (currentH < kEyeClosedH) {
    currentH = kEyeClosedH;
  }
  animateEyes(currentH, kEyeClosedH, kBlinkCloseMs, lv_anim_path_ease_in,
              onBlinkClosed);
  CFG_LOG("[ELF] blink%s", m_blinkIsSecond ? " x2" : "");
}

void ElfOverlay::startGaze() {
  if (m_state != State::Idle) {
    return;
  }
#if CFG_ELF_SOUND_ENABLE
  // Grooving（含退出淡出）由拍点换向接管视线；Listening 允许随机注视。
  if (m_soundLevel == SoundLevel::Grooving || m_grooveW > 0.001f) {
    m_nextGazeAt = millis() + randomBetween(CFG_ELF_GAZE_MIN_MS,
                                            CFG_ELF_GAZE_MAX_MS);
    return;
  }
#endif
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
  // 整个身体跟着视线轻晃，放大可见的左右位移。
  const int32_t leanX = offsetX * kGazeLeanX / kGazeRangeX;
  startAnim(m_body, setBodyX, lv_obj_get_x(m_body), leanX, kGazeMs,
            lv_anim_path_ease_out, nullptr);

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
#if CFG_ELF_SOUND_ENABLE
  // 触摸优先级最高：声音层让位 600ms，期间不写身体 x/y 与虹膜。
  m_soundYieldUntil = millis() + kSoundYieldMs;
#endif

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

  lv_anim_delete(m_body, setBodyX);
  setBodyX(m_body,
           clampInt(dx * kGazeLeanX / (kScreenSize / 2), -kGazeLeanX,
                    kGazeLeanX));
}

void ElfOverlay::touchEnd() {
  if (!m_built) {
    return;
  }
  m_lastInteractionAt = millis();
#if CFG_ELF_SOUND_ENABLE
  // 松手后再让声音层等 600ms，和随机注视的恢复时间一致。
  m_soundYieldUntil = m_lastInteractionAt + kSoundYieldMs;
#endif
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
#if CFG_ELF_SOUND_ENABLE
    // 声音层活跃时瞳孔只有一个写入者：只更新基准，交给 updatePupilTarget()。
    if (soundTintActive()) {
      updatePupilTarget();
    } else
#endif
    {
      lv_anim_delete(this, setPupilSize);
      if (m_state == State::Hidden) {
        setPupilSize(this, target);
      } else if (m_state != State::Hiding && m_state != State::Happy) {
        // Happy 期间不抢瞳孔动画，结束时自然回到新的基准值。
        startAnim(this, setPupilSize, fromSize, target, kPupilAnimMs,
                  lv_anim_path_ease_out, nullptr);
      }
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
#if CFG_ELF_SOUND_ENABLE
  // 睡眠接管：先归还声音层占用的属性，避免两套动画同写 m_body.y。
  resetSoundVisuals(false);
#endif
  m_state = State::Sleepy;
  m_blinkPhase = BlinkPhase::None;
  lv_anim_delete(this, setEyeOpen);
  lv_anim_delete(this, setGazeX);
  lv_anim_delete(this, setGazeY);

  const int32_t currentH = lv_obj_get_height(m_eye[0]);
  animateEyes(currentH, kEyeSleepyH, 400, lv_anim_path_ease_in_out, nullptr);
  startAnim(this, setGazeY, lv_obj_get_y(m_iris[0]),
            kIrisY + kSleepyIrisDrop, 500, lv_anim_path_ease_out, nullptr);
  lv_anim_delete(m_body, setBodyX);
  startAnim(m_body, setBodyX, lv_obj_get_x(m_body), 0, 400,
            lv_anim_path_ease_out, nullptr);
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
  lv_anim_delete(m_body, setBodyX);
  startAnim(m_body, setBodyX, lv_obj_get_x(m_body), 0, 300,
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
#if CFG_ELF_SOUND_ENABLE
  resetSoundVisuals(false);
#endif
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
#if CFG_ELF_SOUND_ENABLE
  resetSoundVisuals(false);
#endif
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
#if CFG_ELF_SOUND_ENABLE
  resetSoundVisuals(false);
#endif
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
    bool sleepDue = due(m_lastInteractionAt + CFG_ELF_SLEEP_MS);
#if CFG_ELF_SOUND_ENABLE && CFG_ELF_SOUND_KEEP_AWAKE
    // 听到声音视为仍有人在旁边：从最后一次有效声音重新计时。
    if (sleepDue && m_lastSoundAt != 0 &&
        !due(m_lastSoundAt + CFG_ELF_SLEEP_MS)) {
      sleepDue = false;
    }
#endif
    if (sleepDue) {
      enterSleepy();
    } else if (m_charging && m_nextYawnAt != 0 && due(m_nextYawnAt)) {
      startYawn();
    } else if (due(m_nextBlinkAt)) {
      startBlink();
    } else if (due(m_nextGazeAt)) {
#if CFG_ELF_SOUND_ENABLE
      if (m_soundLevel == SoundLevel::Grooving || m_grooveW > 0.001f) {
        // 律动（含淡出）期间暂停随机注视，改用拍点换向的视线。
        m_nextGazeAt = now + randomBetween(CFG_ELF_GAZE_MIN_MS,
                                           CFG_ELF_GAZE_MAX_MS);
      } else
#endif
      {
        startGaze();
      }
    }
    return;
  }

  if (m_state == State::Sleepy &&
      due(m_lastInteractionAt + CFG_ELF_SLEEP_DEEP_MS)) {
    enterSleeping();
  }
}

// ---------------------------------------------------------------------------
// 阶段 5：声音层（30ms tick）
// ---------------------------------------------------------------------------

void ElfOverlay::setSoundFrame(const SoundFrame& frame) {
#if CFG_ELF_SOUND_ENABLE
  if (!m_built || m_state == State::Hidden || m_state == State::Hiding) {
    return;
  }
  const uint32_t now = millis();
  m_level = frame.level;
  m_low = frame.low;
  m_bpm = frame.bpm;
  m_confidence = frame.confidence;
  m_loudFlag = frame.loud;
  m_soundHadFrame = true;
  m_soundFrameAt = now;
  // 主循环可能快于 30ms：先把拍点挂起，交给 tick 统一消费。
  if (frame.beat) {
    m_pendingBeat = true;
  }
#else
  (void)frame;
#endif
}

bool ElfOverlay::soundTintActive() const {
#if CFG_ELF_SOUND_ENABLE
  return m_soundTintApplied &&
         (m_soundLevel == SoundLevel::Listening ||
          m_soundLevel == SoundLevel::Grooving) &&
         (m_state == State::Idle || m_state == State::Blinking);
#else
  return false;
#endif
}

// 瞳孔的唯一写入者：车速/Happy 基准 + 声音偏移，统一钳制上限。
void ElfOverlay::updatePupilTarget() {
#if CFG_ELF_SOUND_ENABLE
  if (!m_built) {
    return;
  }
  int32_t base = m_pupilBaseSize;
  if (m_state == State::Happy && base < kPupilHappySize) {
    base = kPupilHappySize;
  }
  int32_t offset = 0;
  if (m_soundLevel == SoundLevel::Grooving) {
    offset = static_cast<int32_t>(6.0f * m_g + 4.0f * m_pulse + 0.5f);
  } else if (m_soundLevel == SoundLevel::Listening) {
    offset = 4 + 2 * static_cast<int32_t>(m_level) / 255;
  }
  int32_t target = base + offset;
  if (target > kPupilSoundMaxSize) {
    target = kPupilSoundMaxSize;
  }
  // 声音层活跃时直接写，避免和车速/Happy 的 lv_anim 抢同一属性。
  lv_anim_delete(this, setPupilSize);
  if (target != m_lastPupilSize) {
    setPupilSize(this, target);
    m_lastPupilSize = target;
  }
#endif
}

void ElfOverlay::handleSoundBeat(uint32_t now) {
#if CFG_ELF_SOUND_ENABLE
  m_lastBeatAt = now;
  m_pulse = 1.0f;
  if (now - m_beatWindowStart > kBeatWindowMs) {
    m_beatWindowStart = now;
    m_beatsInWindow = 0;
  }
  if (m_beatsInWindow < 1000) {
    ++m_beatsInWindow;
  }
  // 每拍换向：身体摇摆与 Grooving 视线共用同一个方向。
  m_soundSwayDir = (m_soundSwayDir >= 0) ? -1 : 1;

  if (m_soundLevel == SoundLevel::Grooving) {
    ++m_beatInGroove;
    emitMusicBubble(now);
    // 每 4 拍快速单眨一次（复用现有眨眼状态机）。
    if ((m_beatInGroove % 4) == 0 && m_state == State::Idle &&
        static_cast<int32_t>(now - m_soundYieldUntil) >= 0) {
      m_nextBlinkAt = now;
    }
    const bool everyEighthBeat = (m_beatInGroove % 8) == 0;
    // 每 8 拍或快歌：本拍 g 临时 ×1.3；快歌每 4 拍最多强制连眨一次。
    if (everyEighthBeat || m_bpm > 150) {
      m_grooveBoostUntil = now + kGrooveBoostMs;
      if (everyEighthBeat || (m_beatInGroove % 4) == 0) {
        m_forceDoubleBlink = true;
      }
    }
  }
#endif
}

void ElfOverlay::enterGroove(uint32_t now) {
#if CFG_ELF_SOUND_ENABLE
  m_soundLevel = SoundLevel::Grooving;
  m_lastBeatAt = now;
  m_beatInGroove = 0;
  m_grooveW = 0.0f;
  // 从当前实际位置接上，避免接管瞬间跳变。
  m_soundBodyX = static_cast<float>(lv_obj_get_x(m_body));
  m_lastBodyX = lv_obj_get_x(m_body);
  m_soundGazeX = static_cast<float>(lv_obj_get_x(m_iris[0]));
  m_soundGazeY = static_cast<float>(lv_obj_get_y(m_iris[0]));
  m_soundSwayDir = 1;
  // 删除随机注视动画；呼吸由 applySoundVisuals() 首次接管时平滑收尾。
  lv_anim_delete(this, setGazeX);
  lv_anim_delete(this, setGazeY);
  lv_anim_delete(m_body, setBodyX);
  CFG_LOG("[ELF] sound groove (bpm=%u level=%u)",
          static_cast<unsigned>(m_bpm), static_cast<unsigned>(m_level));
#endif
}

void ElfOverlay::resetSoundState() {
#if CFG_ELF_SOUND_ENABLE
  m_soundLevel = SoundLevel::Off;
  m_pendingBeat = false;
  m_forceDoubleBlink = false;
  m_pulse = 0.0f;
  m_soundYOffset = 0.0f;
  m_g = 0.0f;
  m_grooveW = 0.0f;
  m_soundBodyX = 0.0f;
  m_soundGazeX = -10000.0f;
  m_soundGazeY = -10000.0f;
  m_soundSwayDir = 1;
  m_beatsInWindow = 0;
  m_beatInGroove = 0;
  m_beatWindowStart = 0;
  m_lastBeatAt = 0;
  m_lastSoundLoudAt = 0;
  m_lastSoundAt = 0;
  m_grooveBoostUntil = 0;
  m_soundYieldUntil = 0;
  m_soundHadFrame = false;
  m_soundFrameAt = 0;
  m_level = 0;
  m_low = 0;
  m_bpm = 0;
  m_confidence = 0;
  m_loudFlag = false;
  m_soundTintApplied = false;
  m_soundVisApplied = false;
  m_soundBreathStopped = false;
  m_nextBubble = 0;
  hideMusicBubbles();
  m_lastBodyX = -10000;
  m_lastBodyY = -10000;
  m_lastEyeH = -10000;
  m_lastMouthH = -10000;
  for (int i = 0; i < 2; ++i) {
    m_lastEarY[i] = -10000;
  }
  for (int i = 0; i < 3; ++i) {
    m_lastTailSize[i] = -10000;
    m_lastTailX[i] = -10000;
    m_lastTailY[i] = -10000;
  }
  m_lastPupilSize = -10000;
  m_lastBlushOpa = -10000;
  m_lastAuraOpa = -10000;
  m_lastSoundHue = -1;
  m_lastGazeX = -10000;
  m_lastGazeY = -10000;
#endif
}

// 把所有声音偏移恢复到阶段 4 基准；hide()/进入其它主状态时立即调用。
// 退出 Grooving 的柔和过渡由 applySoundVisuals() 的 grooveW 负责。
void ElfOverlay::resetSoundVisuals(bool animate) {
#if CFG_ELF_SOUND_ENABLE
  (void)animate;
  const bool allowed =
      (m_state == State::Idle || m_state == State::Blinking);
  const bool yield =
      static_cast<int32_t>(millis() - m_soundYieldUntil) < 0;
  m_grooveW = 0.0f;
  m_soundBodyX = 0.0f;
  m_soundYOffset = 0.0f;
  m_soundVisApplied = false;

  // 腮红 / 瞳孔 / 光环无论当前主状态如何都恢复基准，避免残留声音亮度。
  lv_anim_delete(this, setPupilSize);
  setPupilSize(this, m_pupilBaseSize);
  m_lastPupilSize = m_pupilBaseSize;
  lv_anim_delete(this, setBlushOpa);
  setBlushOpa(this, kBlushOpa);
  m_lastBlushOpa = kBlushOpa;
  lv_obj_set_style_border_opa(m_aura, kAuraBaseOpa, 0);
  lv_obj_set_style_border_color(m_aura, kGlow, 0);
  m_lastAuraOpa = kAuraBaseOpa;
  m_lastSoundHue = -1;
  m_soundTintApplied = false;

  for (int i = 0; i < 2; ++i) {
    lv_obj_set_y(m_ear[i], kEarY);
    m_lastEarY[i] = kEarY;
    lv_obj_set_style_bg_color(m_ear[i], kSilhouette, 0);
  }
  for (int i = 0; i < 3; ++i) {
    lv_obj_set_pos(m_tail[i], kTailX[i], kTailY[i]);
    lv_obj_set_size(m_tail[i], kTailSize[i], kTailSize[i]);
    lv_obj_set_style_bg_color(m_tail[i], kSilhouette, 0);
    m_lastTailSize[i] = kTailSize[i];
    m_lastTailX[i] = kTailX[i];
    m_lastTailY[i] = kTailY[i];
  }
  hideMusicBubbles();

  if (!allowed) {
    return;
  }

  if (m_soundBreathStopped) {
    m_soundBreathStopped = false;
    resumeBreathing();
  }
  if (!yield) {
    lv_anim_delete(m_body, setBodyX);
    setBodyX(m_body, 0);
    m_lastBodyX = 0;
  }

  if (m_state == State::Idle) {
    setEyeOpen(this, kEyeH);
    m_lastEyeH = kEyeH;
  }

  lv_obj_set_size(m_mouth, kMouthW, kMouthH);
  lv_obj_set_y(m_mouth, kMouthCenterY - kMouthH / 2);
  m_lastMouthH = kMouthH;

  // 触摸让位期间不写虹膜，避免抢 lookAt() 的追随位置。
  if (!yield) {
    lv_anim_delete(this, setGazeX);
    lv_anim_delete(this, setGazeY);
    setGazeX(this, kIrisInset);
    setGazeY(this, kIrisY);
    m_lastGazeX = kIrisInset;
    m_lastGazeY = kIrisY;
  }
#endif
}

void ElfOverlay::updateSoundState(uint32_t now) {
#if CFG_ELF_SOUND_ENABLE
  if (m_state == State::Hidden || m_state == State::Hiding) {
    if (m_soundLevel != SoundLevel::Off) {
      m_soundLevel = SoundLevel::Off;
    }
    return;
  }

  const bool fresh = (now - m_soundFrameAt) <= 1000;
  const uint8_t level = fresh ? m_level : 0;
  const bool loud = fresh &&
                    (level >= CFG_ELF_SOUND_NOISE_GATE || m_loudFlag);
  if (loud) {
    m_lastSoundLoudAt = now;
#if CFG_ELF_SOUND_KEEP_AWAKE
    // 有效声音 = 有人在旁边：刷新困倦计时；困倦/睡着的精灵直接唤醒，
    // 醒来后由 Listening/Grooving 层自然接管。
    m_lastSoundAt = now;
    if (m_state == State::Sleepy || m_state == State::Sleeping) {
      wakeFromSleep();
      CFG_LOG("[ELF] wake by sound (bpm=%u level=%u)",
              static_cast<unsigned>(m_bpm),
              static_cast<unsigned>(m_level));
    }
#endif
  }

  // 拍点窗口过期，避免陈旧计数误进 Grooving。
  if (now - m_beatWindowStart > kBeatWindowMs) {
    m_beatWindowStart = now;
    m_beatsInWindow = 0;
  }

  const bool bpmOk = (m_bpm >= CFG_ELF_SOUND_BEAT_MIN_BPM &&
                      m_bpm <= CFG_ELF_SOUND_BEAT_MAX_BPM);

  switch (m_soundLevel) {
    case SoundLevel::Off:
      if (m_soundHadFrame && fresh) {
        m_soundLevel = SoundLevel::Quiet;
        if (m_lastSoundLoudAt == 0) {
          m_lastSoundLoudAt = now;
        }
        CFG_LOG("[ELF] sound quiet (bpm=%u level=%u)",
                static_cast<unsigned>(m_bpm),
                static_cast<unsigned>(m_level));
      }
      break;
    case SoundLevel::Quiet:
      if (loud) {
        m_soundLevel = SoundLevel::Listening;
        m_lastSoundLoudAt = now;
        CFG_LOG("[ELF] sound listening (bpm=%u level=%u)",
                static_cast<unsigned>(m_bpm),
                static_cast<unsigned>(m_level));
      }
      break;
    case SoundLevel::Listening:
      if (!loud &&
          now - m_lastSoundLoudAt >= CFG_ELF_SOUND_QUIET_MS) {
        m_soundLevel = SoundLevel::Quiet;
        CFG_LOG("[ELF] sound quiet (bpm=%u level=%u)",
                static_cast<unsigned>(m_bpm),
                static_cast<unsigned>(m_level));
        break;
      }
      if (m_beatsInWindow >= CFG_ELF_SOUND_BEAT_CONFIRM && bpmOk &&
          m_confidence >= CFG_ELF_SOUND_MIN_CONFIDENCE) {
        enterGroove(now);
      }
      break;
    case SoundLevel::Grooving:
      if (now - m_lastBeatAt >= CFG_ELF_SOUND_GROOVE_HOLD_MS) {
        m_soundLevel = SoundLevel::Listening;
        CFG_LOG("[ELF] sound groove end (bpm=%u level=%u)",
                static_cast<unsigned>(m_bpm),
                static_cast<unsigned>(m_level));
      }
      break;
  }
#endif
}

void ElfOverlay::applySoundVisuals(uint32_t now, uint32_t dt) {
#if CFG_ELF_SOUND_ENABLE
  if (!m_built) {
    return;
  }
  const bool allowed =
      (m_state == State::Idle || m_state == State::Blinking);
  if (!allowed) {
    // 主状态（Happy/哈欠/眯眼/困倦/入睡/隐藏）接管画面：
    // 只清声音层的“所有权”，不写属性，避免和主状态动画互相抢。
    m_grooveW = 0.0f;
    m_soundBodyX = 0.0f;
    m_soundVisApplied = false;
    m_soundTintApplied = false;
    m_soundBreathStopped = false;
    m_soundYOffset = 0.0f;
    return;
  }

  if (dt > 200) {
    dt = 200;
  }
  const bool grooving = (m_soundLevel == SoundLevel::Grooving);
  const bool listening = (m_soundLevel == SoundLevel::Listening);
  const bool tint = grooving || listening;
  const bool yield =
      static_cast<int32_t>(now - m_soundYieldUntil) < 0;
  const uint8_t level =
      (now - m_soundFrameAt <= 1000) ? m_level : 0;

  // g = max(level/255, bpm 权重)，平滑后驱动幅度。
  float targetG = static_cast<float>(level) / 255.0f;
  float bpmG = 0.0f;
  if (m_bpm >= 140) {
    bpmG = 0.7f;
  } else if (m_bpm > 60) {
    bpmG = 0.7f * static_cast<float>(m_bpm - 60) / 80.0f;
  }
  if (bpmG > targetG) {
    targetG = bpmG;
  }
  if (static_cast<int32_t>(now - m_grooveBoostUntil) < 0) {
    targetG *= 1.3f;
  }
  if (targetG > 1.0f) {
    targetG = 1.0f;
  }
  float gk = static_cast<float>(dt) / 120.0f;
  if (gk > 1.0f) {
    gk = 1.0f;
  }
  m_g += (targetG - m_g) * gk;

  // groove 权重：进入 150ms 升到 1，退出 500ms 平滑归零。
  if (grooving) {
    m_grooveW += static_cast<float>(dt) /
                 static_cast<float>(kGrooveFadeInMs);
    if (m_grooveW > 1.0f) {
      m_grooveW = 1.0f;
    }
  } else if (m_grooveW > 0.0f) {
    m_grooveW -= static_cast<float>(dt) /
                 static_cast<float>(kGrooveFadeOutMs);
    if (m_grooveW < 0.0f) {
      m_grooveW = 0.0f;
    }
  }
  const bool grooveOwns = (m_grooveW > 0.0f);
  if (grooveOwns) {
    m_soundVisApplied = true;
    if (!m_soundBreathStopped) {
      m_soundYOffset = static_cast<float>(lv_obj_get_y(m_body));
      stopBreathing();
      m_soundBreathStopped = true;
      m_lastBodyY = -10000;
    }
  }
  if (m_soundYOffset != 0.0f) {
    m_soundYOffset *= expf(-static_cast<float>(dt) / 95.0f);
    if (fabsf(m_soundYOffset) < 0.5f) {
      m_soundYOffset = 0.0f;
    }
  }

  float sx = static_cast<float>(dt) / 50.0f;
  if (sx > 1.0f) {
    sx = 1.0f;
  }

  // 身体：y = 呼吸基准(0) − 弹跳；x = 拍点换向的摇摆。
  if (grooveOwns) {
    const int32_t bounce =
        yield ? 0 : static_cast<int32_t>(
                         (4.0f +
                          static_cast<float>(CFG_ELF_SOUND_BOUNCE_PX - 4) *
                              m_g) *
                             m_pulse * m_grooveW +
                         0.5f);
    const int32_t wantY =
        -bounce + static_cast<int32_t>(lroundf(m_soundYOffset));
    if (wantY != m_lastBodyY) {
      setBodyY(m_body, wantY);
      m_lastBodyY = wantY;
    }
    if (!yield) {
      const float targetX = static_cast<float>(m_soundSwayDir) *
                            (4.0f +
                             static_cast<float>(CFG_ELF_SOUND_SWAY_PX - 4) *
                                 m_g) *
                            m_grooveW;
      m_soundBodyX += (targetX - m_soundBodyX) * sx;
      const int32_t wantX =
          static_cast<int32_t>(m_soundBodyX >= 0.0f ? m_soundBodyX + 0.5f
                                                    : m_soundBodyX - 0.5f);
      if (wantX != m_lastBodyX) {
        setBodyX(m_body, wantX);
        m_lastBodyX = wantX;
      }
    }
  }
  if (yield) {
    // 让位期间跟随触摸写入的实际值，600ms 后恢复时不跳变。
    m_soundBodyX = static_cast<float>(lv_obj_get_x(m_body));
    m_lastBodyX = lv_obj_get_x(m_body);
    m_soundGazeX = static_cast<float>(lv_obj_get_x(m_iris[0]));
    m_soundGazeY = static_cast<float>(lv_obj_get_y(m_iris[0]));
    m_lastGazeX = lv_obj_get_x(m_iris[0]);
    m_lastGazeY = lv_obj_get_y(m_iris[0]);
  }

  // 开心眼：中心不变；Blinking 期间跳过，避免和眨眼动画抢高度。
  if (m_state == State::Idle) {
    int32_t off = 0;
    if (grooveOwns) {
      off = static_cast<int32_t>(
          (4.0f + 6.0f * m_g) * m_pulse * m_grooveW + 0.5f);
    }
    const int32_t wantH = kEyeH + off;
    if (wantH != m_lastEyeH) {
      setEyeOpen(this, wantH);
      m_lastEyeH = wantH;
    }
  }

  // 嘴高：14 + 10 * level/255，最大 24；中心不变。
  int32_t mouthH = kMouthH;
  if (grooveOwns) {
    mouthH += static_cast<int32_t>(
        10.0f * static_cast<float>(level) / 255.0f * m_grooveW + 0.5f);
  }
  if (mouthH > kMouthMaxH) {
    mouthH = kMouthMaxH;
  }
  if (mouthH != m_lastMouthH) {
    lv_obj_set_size(m_mouth, kMouthW, mouthH);
    lv_obj_set_y(m_mouth, kMouthCenterY - mouthH / 2);
    m_lastMouthH = mouthH;
  }

  // 耳朵随每拍交替弹起，触摸期间不覆盖用户交互。
  const int activeEar = m_soundSwayDir > 0 ? 0 : 1;
  for (int i = 0; i < 2; ++i) {
    const float earLift =
        grooveOwns && !yield
            ? (i == activeEar ? 9.0f : 3.0f) * m_pulse * m_grooveW
            : 0.0f;
    const int32_t earY = kEarY - static_cast<int32_t>(earLift + 0.5f);
    if (earY != m_lastEarY[i]) {
      lv_obj_set_y(m_ear[i], earY);
      m_lastEarY[i] = earY;
    }
  }

  // 尾巴按低频能量伸缩，并随每拍做波浪摆动。
  for (int i = 0; i < 3; ++i) {
    int32_t size = kTailSize[i];
    if (grooveOwns && m_low > i * 80) {
      size += static_cast<int32_t>(
          (2.0f + 4.0f * m_g) * m_grooveW + 0.5f);
    }
    const float wag =
        grooveOwns && !yield ? m_pulse * m_grooveW : 0.0f;
    const int32_t centerX =
        kTailX[i] + kTailSize[i] / 2 +
        static_cast<int32_t>(
            lroundf(m_soundSwayDir * (i + 1) * 2.0f * wag));
    const int32_t centerY =
        kTailY[i] + kTailSize[i] / 2 -
        static_cast<int32_t>(lroundf((3 - i) * 2.0f * wag));
    const int32_t tailX = centerX - size / 2;
    const int32_t tailY = centerY - size / 2;
    if (size != m_lastTailSize[i] || tailX != m_lastTailX[i] ||
        tailY != m_lastTailY[i]) {
      lv_obj_set_pos(m_tail[i], tailX, tailY);
      lv_obj_set_size(m_tail[i], size, size);
      m_lastTailSize[i] = size;
      m_lastTailX[i] = tailX;
      m_lastTailY[i] = tailY;
    }
  }

  // Grooving 视线：拍点换向，±24 轻微摆动；触摸优先时完全不写。
  if (grooveOwns) {
    if (!yield) {
      const int32_t targetX = kIrisInset + m_soundSwayDir * kSoundGazeX;
      float gx = static_cast<float>(dt) / 150.0f;
      if (gx > 1.0f) {
        gx = 1.0f;
      }
      m_soundGazeX += (static_cast<float>(targetX) - m_soundGazeX) * gx;
      m_soundGazeY += (static_cast<float>(kIrisY) - m_soundGazeY) * gx;
      const int32_t ix = static_cast<int32_t>(
          m_soundGazeX >= 0.0f ? m_soundGazeX + 0.5f : m_soundGazeX - 0.5f);
      const int32_t iy = static_cast<int32_t>(
          m_soundGazeY >= 0.0f ? m_soundGazeY + 0.5f : m_soundGazeY - 0.5f);
      if (ix != m_lastGazeX) {
        setGazeX(this, ix);
        m_lastGazeX = ix;
      }
      if (iy != m_lastGazeY) {
        setGazeY(this, iy);
        m_lastGazeY = iy;
      }
    }
  } else if (m_soundVisApplied) {
    // 500ms 归零完成：交还呼吸与随机注视。
    m_soundVisApplied = false;
    if (m_soundBreathStopped) {
      m_soundBreathStopped = false;
      resumeBreathing();
    }
    if (!yield) {
      setBodyX(m_body, 0);
      m_lastBodyX = 0;
      setGazeX(this, kIrisInset);
      setGazeY(this, kIrisY);
      m_lastGazeX = kIrisInset;
      m_lastGazeY = kIrisY;
    }
  }

  // 腮红 / 光环 / 瞳孔：Listening 和 Grooving 都有轻微反应。
  if (tint) {
    m_soundTintApplied = true;
    int32_t blush = kBlushOpa + static_cast<int32_t>(
                                    51.0f * static_cast<float>(level) /
                                        255.0f + 0.5f);
    if (blush > kBlushSoundMaxOpa) {
      blush = kBlushSoundMaxOpa;
    }
    if (blush != m_lastBlushOpa) {
      lv_anim_delete(this, setBlushOpa);
      setBlushOpa(this, blush);
      m_lastBlushOpa = blush;
    }

    updatePupilTarget();
  } else if (m_soundTintApplied) {
    // 回到 Quiet/Off：一次性恢复阶段 4 的腮红与瞳孔基准。
    m_soundTintApplied = false;
    lv_anim_delete(this, setPupilSize);
    setPupilSize(this, m_pupilBaseSize);
    m_lastPupilSize = m_pupilBaseSize;
    lv_anim_delete(this, setBlushOpa);
    setBlushOpa(this, kBlushOpa);
    m_lastBlushOpa = kBlushOpa;
  }

  if (tint) {
    uint16_t hue = 185;
    if (grooving) {
      float beatPhase = 0.0f;
      if (m_bpm > 0) {
        beatPhase = static_cast<float>(now - m_lastBeatAt) *
                    static_cast<float>(m_bpm) / 60000.0f;
        if (beatPhase > 1.0f) {
          beatPhase = 1.0f;
        }
      }
      hue = static_cast<uint16_t>(
          (185 + (m_beatInGroove % 8) * 45 +
           static_cast<int32_t>(45.0f * beatPhase)) %
          360);
    } else {
      hue = static_cast<uint16_t>(
          185 + static_cast<uint16_t>(level) * 110 / 255);
    }
    if (hue != m_lastSoundHue) {
      lv_obj_set_style_border_color(m_aura,
                                    lv_color_hsv_to_rgb(hue, 190, 245), 0);
      for (int i = 0; i < 2; ++i) {
        const uint16_t earHue = static_cast<uint16_t>((hue + i * 18) % 360);
        lv_obj_set_style_bg_color(
            m_ear[i], lv_color_hsv_to_rgb(earHue, 155, 115), 0);
      }
      for (int i = 0; i < 3; ++i) {
        const uint16_t tailHue = static_cast<uint16_t>((hue + i * 16) % 360);
        lv_obj_set_style_bg_color(
            m_tail[i], lv_color_hsv_to_rgb(tailHue, 155, 115), 0);
      }
      m_lastSoundHue = hue;
    }
  } else if (m_lastSoundHue >= 0) {
    lv_obj_set_style_border_color(m_aura, kGlow, 0);
    for (int i = 0; i < 2; ++i) {
      lv_obj_set_style_bg_color(m_ear[i], kSilhouette, 0);
    }
    for (int i = 0; i < 3; ++i) {
      lv_obj_set_style_bg_color(m_tail[i], kSilhouette, 0);
    }
    m_lastSoundHue = -1;
  }

  // 安静时光环也缓慢呼吸；声音律动的亮度叠加其上。
  const float phase =
      static_cast<float>(now % kAuraAmbientPeriodMs) /
      static_cast<float>(kAuraAmbientPeriodMs) * 6.2831853f;
  int32_t aura = kAuraBaseOpa + static_cast<int32_t>(
                                    kAuraAmbientPulseOpa *
                                        (0.5f + 0.5f * sinf(phase)) +
                                    0.5f);
  if (tint) {
    aura += grooving
                ? static_cast<int32_t>(31.0f * m_g + 20.0f * m_pulse + 0.5f)
                : static_cast<int32_t>(
                      10.0f * static_cast<float>(level) / 255.0f + 0.5f);
  }
  if (aura > kAuraSoundMaxOpa) {
    aura = kAuraSoundMaxOpa;
  }
  if (aura != m_lastAuraOpa) {
    lv_obj_set_style_border_opa(m_aura, static_cast<lv_opa_t>(aura), 0);
    m_lastAuraOpa = aura;
  }
#endif
}

void ElfOverlay::onSoundTimer() {
#if CFG_ELF_SOUND_ENABLE
  if (!m_built) {
    return;
  }
  if (m_state == State::Hidden || m_state == State::Hiding) {
    return;
  }
  const uint32_t now = millis();
  if (m_soundLastTick == 0) {
    m_soundLastTick = now;
  }
  uint32_t dt = now - m_soundLastTick;
  m_soundLastTick = now;
  if (dt > 250) {
    dt = 250;  // 长时间挂起后不要一次跳完包络
  }

  if (m_pendingBeat) {
    m_pendingBeat = false;
    handleSoundBeat(now);
  }
  if (m_pulse > 0.0f) {
    m_pulse *= expf(-static_cast<float>(dt) /
                    static_cast<float>(kPulseDecayMs));
    if (m_pulse < 0.005f) {
      m_pulse = 0.0f;
    }
  }

  updateSoundState(now);
  applySoundVisuals(now, dt);
  updateMusicBubbles(now);
#endif
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

void ElfOverlay::soundTimerCallback(lv_timer_t* timer) {
  auto* self = static_cast<ElfOverlay*>(lv_timer_get_user_data(timer));
  if (self != nullptr) {
    self->onSoundTimer();
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

void ElfOverlay::setBodyX(void* var, int32_t x) {
  lv_obj_set_x(static_cast<lv_obj_t*>(var), x);
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
  lv_anim_delete(self->m_body, setBodyX);
  lv_obj_set_x(self->m_body, 0);
  lv_obj_set_hidden(self->m_root, true);
#if CFG_ELF_SOUND_ENABLE
  // 完全隐藏后声音子状态归 Off；下次 show() 等第一帧再进 Quiet。
  self->resetSoundState();
#endif
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
  // 声音 8 拍/快歌要求连眨；否则保持阶段 3 的 20% 随机连眨。
  if (!self->m_blinkIsSecond &&
      (self->m_forceDoubleBlink || randomBetween(0, 99) < 20)) {
    self->m_forceDoubleBlink = false;
    self->m_blinkIsSecond = true;
    self->m_nextBlinkAt = now + kBlinkSecondGapMs;
    self->m_nextGazeAt =
        now + randomBetween(CFG_ELF_GAZE_MIN_MS, CFG_ELF_GAZE_MAX_MS);
    return;
  }
  self->m_forceDoubleBlink = false;
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
void ElfOverlay::setSoundFrame(const SoundFrame&) {}

#endif  // CFG_ELF_ENABLE
