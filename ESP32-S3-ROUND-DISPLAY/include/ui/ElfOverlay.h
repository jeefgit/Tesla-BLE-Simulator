#pragma once

#include <lvgl.h>
#include <stdint.h>

#include "audio/SoundSensor.h"

// 离线小精灵浮层（见 design/ELF-DESIGN.md）。
//
// 对象树在 begin() 时一次性建好，之后只改属性；默认隐藏。
// 由 DashboardApp 负责手势识别，本类内部用 50ms lv_timer 驱动状态机：
// 眨眼、随机注视、呼吸漂移、触摸追随、开心反应、困倦/入睡、充电打哈欠、
// 车速瞳孔/急减速眯眼（阶段 4），声音律动层（阶段 5）。
class ElfOverlay {
 public:
  // 在 active screen 上建对象；必须在 LvglDashboard::begin() 之后调用，
  // 否则会被开机图层盖住。
  void begin();

  // 根容器是否显示（含睁眼/闭眼动画过程）。
  bool visible() const;

  // 横滑呼出 / 退出。
  void show();
  void hide();

  // 触摸移动：眼睛追随手指（屏幕坐标已换算成 LVGL 画面坐标）。
  void lookAt(int16_t screenX, int16_t screenY);

  // 松手：600ms 后回到随机注视。
  void touchEnd();

  // 轻点：开心反应 + 重置自动隐藏计时。
  void poke(int16_t screenX, int16_t screenY);

  // 车辆状态实时数据（本地，不联网）：
  //   speedValid=false 时视为无车速；charging=true 时定期打哈欠。
  void setVehicleState(float speedKmh, bool speedValid, bool charging);

  // 阶段 5：主循环每轮传入最新声音快照；只在可见时调用。
  void setSoundFrame(const SoundFrame& frame);

 private:
  static constexpr uint8_t kMusicBubbleCount = 16;

  // 声音子状态（与主状态机正交，见 design/ELF-DESIGN.md §2.4）。
  enum class SoundLevel : uint8_t { Off, Quiet, Listening, Grooving };

  enum class State : uint8_t {
    Hidden,
    Waking,
    Idle,
    Blinking,
    Happy,
    Yawning,
    Squint,
    Sleepy,
    Sleeping,
    Hiding
  };
  enum class BlinkPhase : uint8_t { None, Closing, Holding, Opening };

  void buildObjects();
  void onTimer();
  void startBreathing(uint32_t halfPeriodMs);
  void stopBreathing();
  // 从当前 y 平滑过渡回呼吸起点（声音层退出时用，避免 22px 跳变）。
  void resumeBreathing();
  void startBlink();
  void startGaze();
  void animateEyes(int32_t fromHeight, int32_t toHeight, uint32_t durationMs,
                   lv_anim_path_cb_t path, lv_anim_ready_cb_t ready);

  void notifyInteraction();
  void enterSleepy();
  void enterSleeping();
  void wakeFromSleep();
  void startHappy();
  void endHappy();
  void resetHappyVisuals();
  void startYawn();
  void startSquint();
  void startZzz();
  void stopZzz();

  // 声音层（30ms 定时器，只做视觉；状态机仍由 50ms 定时器驱动）。
  void onSoundTimer();
  void updateSoundState(uint32_t now);
  void applySoundVisuals(uint32_t now, uint32_t dt);
  void handleSoundBeat(uint32_t now);
  void enterGroove(uint32_t now);
  void emitMusicBubble(uint32_t now);
  void updateMusicBubbles(uint32_t now);
  void hideMusicBubbles();
  void resetSoundVisuals(bool animate);
  void resetSoundState();
  void updatePupilTarget();
  bool soundTintActive() const;

  static void timerCallback(lv_timer_t* timer);
  static void soundTimerCallback(lv_timer_t* timer);
  static void setEyeOpen(void* var, int32_t height);
  static void setGazeX(void* var, int32_t x);
  static void setGazeY(void* var, int32_t y);
  static void setBodyY(void* var, int32_t y);
  static void setBodyYOnSelf(void* var, int32_t y);
  static void onResumeBreathDone(lv_anim_t* anim);
  static void setBodyX(void* var, int32_t x);
  static void setPupilSize(void* var, int32_t size);
  static void setBlushOpa(void* var, int32_t opacity);
  static void setMouthOpa(void* var, int32_t opacity);
  static void setZOpacity(void* var, int32_t progress);
  static void onWakeDone(lv_anim_t* anim);
  static void onHideDone(lv_anim_t* anim);
  static void onBlinkClosed(lv_anim_t* anim);
  static void onBlinkOpened(lv_anim_t* anim);
  static void onYawnClosed(lv_anim_t* anim);
  static void onYawnOpened(lv_anim_t* anim);
  static void onSquintDone(lv_anim_t* anim);

  lv_obj_t* m_root = nullptr;
  lv_obj_t* m_aura = nullptr;
  lv_obj_t* m_body = nullptr;
  lv_obj_t* m_eye[2] = {};
  lv_obj_t* m_iris[2] = {};
  lv_obj_t* m_pupil[2] = {};
  lv_obj_t* m_blush[2] = {};
  lv_obj_t* m_mouth = nullptr;
  lv_obj_t* m_ear[2] = {};
  lv_obj_t* m_tail[3] = {};
  lv_obj_t* m_z[3] = {};
  lv_obj_t* m_bubble[kMusicBubbleCount] = {};
  lv_timer_t* m_timer = nullptr;

  State m_state = State::Hidden;
  BlinkPhase m_blinkPhase = BlinkPhase::None;
  uint32_t m_lastInteractionAt = 0;
  uint32_t m_happyUntil = 0;
  uint32_t m_nextBlinkAt = 0;
  uint32_t m_nextGazeAt = 0;
  uint32_t m_blinkClosedAt = 0;
  uint32_t m_yawnClosedAt = 0;
  bool m_yawnHolding = false;
  uint32_t m_squintUntil = 0;
  uint32_t m_squintCooldownUntil = 0;
  // 充电打哈欠的下一次触发时间；0 = 当前不充电、不排期。
  uint32_t m_nextYawnAt = 0;
  // 车速瞳孔基准（Happy 会临时放大，结束后回到这个值）。
  int32_t m_pupilBaseSize = 74;
  bool m_charging = false;
  bool m_haveSpeedSample = false;
  float m_lastSpeedKmh = 0;
  uint32_t m_lastSpeedAt = 0;
  // 本次眨眼是否是“连眨”的第二下，避免无限连眨。
  bool m_blinkIsSecond = false;
  bool m_built = false;

  // --- 阶段 5：声音律动 ---
  lv_timer_t* m_soundTimer = nullptr;
  SoundLevel m_soundLevel = SoundLevel::Off;
  uint8_t m_level = 0;
  uint8_t m_low = 0;
  uint8_t m_bpm = 0;
  uint8_t m_confidence = 0;
  bool m_loudFlag = false;
  bool m_soundHadFrame = false;
  // 一次性拍点：主循环可能快于 30ms tick，先累计再消费，避免丢拍。
  bool m_pendingBeat = false;
  // 8 拍 / 快歌的下一拍眨眼要连眨两下（一次性）。
  bool m_forceDoubleBlink = false;
  uint32_t m_soundFrameAt = 0;
  uint32_t m_soundLastTick = 0;
  uint32_t m_lastBeatAt = 0;
  uint32_t m_lastSoundLoudAt = 0;
  uint32_t m_beatWindowStart = 0;
  int16_t m_beatsInWindow = 0;
  int16_t m_beatInGroove = 0;
  uint32_t m_grooveBoostUntil = 0;
  uint32_t m_soundYieldUntil = 0;
  // pulse：拍点衰减包络（1 → 0）；g：律动强度（0..1）。
  float m_pulse = 0;
  float m_soundYOffset = 0.0f;  // 声音层接管呼吸时的 body.y 残留偏移
  float m_g = 0;
  // grooveW：Grooving 偏移权重，进入快速升到 1，退出 500ms 降到 0。
  float m_grooveW = 0;
  float m_soundBodyX = 0;
  float m_soundGazeX = -10000;
  float m_soundGazeY = -10000;
  int8_t m_soundSwayDir = 1;
  uint32_t m_bubbleStartedAt[kMusicBubbleCount] = {};
  int16_t m_bubbleStartX[kMusicBubbleCount] = {};
  int16_t m_bubbleStartY[kMusicBubbleCount] = {};
  uint16_t m_bubbleHue[kMusicBubbleCount] = {};
  uint8_t m_bubbleBaseSize[kMusicBubbleCount] = {};
  uint8_t m_nextBubble = 0;
  bool m_bubbleActive[kMusicBubbleCount] = {};
  bool m_soundTintApplied = false;
  bool m_soundVisApplied = false;
  bool m_soundBreathStopped = false;

  // 属性缓存：只在值变化时调用 lv_obj_set_*，避免无谓重绘。
  int32_t m_lastBodyX = -10000;
  int32_t m_lastBodyY = -10000;
  int32_t m_lastEyeH = -10000;
  int32_t m_lastMouthH = -10000;
  int32_t m_lastEarY[2] = {-10000, -10000};
  int32_t m_lastTailSize[3] = {-10000, -10000, -10000};
  int32_t m_lastTailX[3] = {-10000, -10000, -10000};
  int32_t m_lastTailY[3] = {-10000, -10000, -10000};
  int32_t m_lastPupilSize = -10000;
  int32_t m_lastBlushOpa = -10000;
  int32_t m_lastAuraOpa = -10000;
  int32_t m_lastSoundHue = -1;
  int32_t m_lastGazeX = -10000;
  int32_t m_lastGazeY = -10000;
};
