#pragma once

#include <stdint.h>

#include "config.h"
#include "core/UiModel.h"

class Qmi8658;
class RoundDisplay;

// 静止自动休眠状态机：
//   - 轮询 QMI8658，维护“最后一次运动/用户操作”时间；
//   - 持续静止达到 CFG_AUTO_SLEEP_IDLE_MS 后熄屏、配置 WoM、进入 deep sleep；
//   - 深睡由 QMI8658 INT2（GPIO21）唤醒，唤醒后重新跑固件。
// 运动只由 IMU 判断，不读取车辆速度。
class PowerManager {
 public:
  // 在显示/UI 初始化之前调用，只读取本次唤醒原因。
  void detectWakeReason();

  void begin(Qmi8658* imu, RoundDisplay* display);

  // 触摸 / BOOT / PWR 等用户操作会重置静止计时。
  void notifyUserActivity();

  void update(const UiModel& model, bool bootAnimationRunning, bool charging);

  // 本次启动是否由 QMI8658 INT2（运动）唤醒。
  bool wokeByMotion() const { return m_wokeByMotion; }

 private:
  void pollImu(uint32_t now);
  void enterDeepSleep();

  Qmi8658* m_imu = nullptr;
  RoundDisplay* m_display = nullptr;
  bool m_imuAvailable = false;
  bool m_enteringSleep = false;
  bool m_wokeByExt1 = false;
  bool m_wokeByMotion = false;

  uint32_t m_lastMotionMs = 0;
  uint32_t m_lastImuPollMs = 0;
  uint8_t m_motionSamples = 0;

  // 重力低通估计，用 |acc - gravity| 判断线性加速度/姿态变化。
  float m_gravityX = 0.0f;
  float m_gravityY = 0.0f;
  float m_gravityZ = 0.0f;
  bool m_gravityReady = false;

  // 运动唤醒后的确认窗口：窗口内没有真实运动则重新休眠。
  bool m_wakeConfirmPending = false;
  uint32_t m_wakeConfirmDeadline = 0;
  bool m_sawMotionSinceWake = false;
};
