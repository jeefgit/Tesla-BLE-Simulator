#pragma once

#include <lvgl.h>
#include <stdint.h>

// 离线小精灵浮层（见 design/ELF-DESIGN.md）。
//
// 对象树在 begin() 时一次性建好，之后只改属性；默认隐藏。
// 由 DashboardApp 负责手势识别，本类内部用 50ms lv_timer 驱动状态机：
// 眨眼、随机注视、呼吸漂移、触摸追随、开心反应、困倦/入睡、充电打哈欠、
// 车速瞳孔/急减速眯眼（阶段 4）。
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

 private:
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

  static void timerCallback(lv_timer_t* timer);
  static void setEyeOpen(void* var, int32_t height);
  static void setGazeX(void* var, int32_t x);
  static void setGazeY(void* var, int32_t y);
  static void setBodyY(void* var, int32_t y);
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
};
