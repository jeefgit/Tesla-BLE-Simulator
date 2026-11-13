#include "power/PowerManager.h"

#include <Arduino.h>
#include <esp_sleep.h>

#include <math.h>

#include "display/RoundDisplay.h"
#include "input/Qmi8658.h"

namespace {

// 重力估计的低通系数：越小越平滑，但姿态变化后恢复得更慢。
constexpr float kGravityLpfAlpha = 0.1f;

const char* wakeCauseName(esp_sleep_wakeup_cause_t cause) {
  switch (cause) {
    case ESP_SLEEP_WAKEUP_EXT1:
      return "EXT1";
    case ESP_SLEEP_WAKEUP_TIMER:
      return "TIMER";
    case ESP_SLEEP_WAKEUP_UNDEFINED:
      return "power-on";
    default:
      return "other";
  }
}

}  // namespace

void PowerManager::detectWakeReason() {
  const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  m_wokeByExt1 = cause == ESP_SLEEP_WAKEUP_EXT1;
  m_wokeByMotion = false;
  if (m_wokeByExt1) {
    const uint64_t status = esp_sleep_get_ext1_wakeup_status();
    m_wokeByMotion =
        (status & (1ULL << CFG_AUTO_SLEEP_WAKE_GPIO)) != 0;
  }

  if (cause != ESP_SLEEP_WAKEUP_UNDEFINED) {
    CFG_LOG("[PWR] wake cause=%s motion=%d", wakeCauseName(cause),
            m_wokeByMotion ? 1 : 0);
  }
}

void PowerManager::begin(Qmi8658* imu, RoundDisplay* display) {
  m_imu = imu;
  m_display = display;
  m_imuAvailable = imu != nullptr && imu->isPresent();

  const uint32_t now = millis();
  m_lastMotionMs = now;
  m_lastImuPollMs = now;

  if (m_wokeByExt1 && m_imuAvailable) {
    // 清除 WoM 事件；DashboardApp 在初始化 IMU 时已软复位回普通模式。
    m_imu->clearWakeStatus();
  }

  if (m_wokeByMotion && CFG_AUTO_SLEEP_WAKE_CONFIRM_MS > 0) {
    m_wakeConfirmPending = true;
    m_wakeConfirmDeadline = now + CFG_AUTO_SLEEP_WAKE_CONFIRM_MS;
    m_sawMotionSinceWake = false;
  }

  if (CFG_AUTO_SLEEP_ENABLE && !m_imuAvailable) {
    // IMU 不可用时不能安全休眠：deep sleep 后没有任何运动唤醒源，
    // 设备会一直睡到手动复位。宁可保持亮屏。
    CFG_LOG("[PWR] IMU unavailable; auto sleep disabled");
  }
}

void PowerManager::notifyUserActivity() {
  m_lastMotionMs = millis();
  m_motionSamples = 0;
  if (m_wakeConfirmPending) {
    m_sawMotionSinceWake = true;
  }
}

void PowerManager::pollImu(uint32_t now) {
#if CFG_AUTO_SLEEP_IMU_ENABLE
  if (!m_imuAvailable) {
    return;
  }
  if (now - m_lastImuPollMs < CFG_AUTO_SLEEP_IMU_POLL_MS) {
    return;
  }
  m_lastImuPollMs = now;

  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  if (!m_imu->readAccel(x, y, z)) {
    return;
  }

  if (!m_gravityReady) {
    m_gravityX = x;
    m_gravityY = y;
    m_gravityZ = z;
    m_gravityReady = true;
    return;
  }

  // 与慢速重力估计的向量差，能同时感知“晃动”和“缓慢翻转”。
  const float dx = x - m_gravityX;
  const float dy = y - m_gravityY;
  const float dz = z - m_gravityZ;
  const float deltaMg = sqrtf(dx * dx + dy * dy + dz * dz) * 1000.0f;

  m_gravityX += (x - m_gravityX) * kGravityLpfAlpha;
  m_gravityY += (y - m_gravityY) * kGravityLpfAlpha;
  m_gravityZ += (z - m_gravityZ) * kGravityLpfAlpha;

  if (deltaMg >= CFG_AUTO_SLEEP_MOTION_THRESHOLD_MG) {
    if (m_motionSamples < 255) {
      ++m_motionSamples;
    }
    if (m_motionSamples >= CFG_AUTO_SLEEP_MOTION_CONFIRM_SAMPLES) {
      m_lastMotionMs = now;
      m_motionSamples = 0;
      m_sawMotionSinceWake = true;
    }
  } else {
    m_motionSamples = 0;
  }
#else
  (void)now;
#endif
}

void PowerManager::update(const UiModel& model, bool bootAnimationRunning,
                          bool charging) {
  if (m_enteringSleep) {
    return;
  }
  const uint32_t now = millis();
  pollImu(now);

  if (!CFG_AUTO_SLEEP_ENABLE || !m_imuAvailable) {
    return;
  }
  // 启动动画 / 配钥流程 / 充电（按配置）期间不允许休眠。
  if (bootAnimationRunning) {
    return;
  }
  if (model.pairing != UiPairingPhase::Idle) {
    return;
  }
#if CFG_AUTO_SLEEP_WHILE_CHARGING == 0
  if (charging) {
    return;
  }
#else
  (void)charging;
#endif

  // 运动唤醒后的确认窗口：窗口内没有任何运动则重新休眠，避免碰一下就
  // 长时间亮屏；有运动则转入正常的静止计时。
  if (m_wakeConfirmPending) {
    if (m_sawMotionSinceWake) {
      m_wakeConfirmPending = false;
      m_lastMotionMs = now;
    } else if (static_cast<int32_t>(now - m_wakeConfirmDeadline) >= 0) {
      CFG_LOG("[PWR] no motion after wake; sleeping again");
      m_wakeConfirmPending = false;
      enterDeepSleep();
      return;
    } else {
      return;
    }
  }

  if (now - m_lastMotionMs >= CFG_AUTO_SLEEP_IDLE_MS) {
    CFG_LOG("[PWR] idle for %lus -> deep sleep",
            static_cast<unsigned long>(CFG_AUTO_SLEEP_IDLE_MS / 1000UL));
    enterDeepSleep();
  }
}

void PowerManager::enterDeepSleep() {
  if (m_enteringSleep || !m_imuAvailable) {
    return;
  }
  m_enteringSleep = true;

  const uint8_t savedBrightness =
      m_display != nullptr ? m_display->brightness() : 0;

  // 1. 熄屏：亮度先归零，再让面板进入 sleep-in。
  if (m_display != nullptr) {
    m_display->setBrightnessRaw(0);
    m_display->panelSleep(true);
  }

  // 2. 配置 QMI8658 WoM。
  if (!m_imu->configureWakeOnMotion(CFG_AUTO_SLEEP_WOM_THRESHOLD_MG,
                                    CFG_AUTO_SLEEP_WOM_BLANKING)) {
    CFG_LOG("[PWR] WoM config failed; stay awake");
    if (m_display != nullptr) {
      m_display->panelSleep(false);
      m_display->setBrightnessRaw(savedBrightness);
    }
    m_enteringSleep = false;
    // 至少等一个静止周期再重试，避免每个 loop 都反复熄屏/亮屏。
    m_lastMotionMs = millis();
    return;
  }

  // 3. 确认 INT2 已回到空闲低电平，否则 ext1 ANY_HIGH 会立刻唤醒，
  //    形成“睡下就醒”的循环。
  pinMode(CFG_AUTO_SLEEP_WAKE_GPIO, INPUT);
  delay(5);
  if (digitalRead(CFG_AUTO_SLEEP_WAKE_GPIO) == HIGH) {
    m_imu->clearWakeStatus();
    delay(10);
  }
  if (digitalRead(CFG_AUTO_SLEEP_WAKE_GPIO) == HIGH) {
    CFG_LOG("[PWR] INT2 still high; abort sleep to avoid wake loop");
    if (m_display != nullptr) {
      m_display->panelSleep(false);
      m_display->setBrightnessRaw(savedBrightness);
    }
    m_enteringSleep = false;
    m_lastMotionMs = millis();
    return;
  }

  // 4. 配置唤醒源：QMI8658 INT2 运动时拉高。
  const esp_err_t wakeErr =
      esp_sleep_enable_ext1_wakeup(1ULL << CFG_AUTO_SLEEP_WAKE_GPIO,
                                   ESP_EXT1_WAKEUP_ANY_HIGH);
  if (wakeErr != ESP_OK) {
    CFG_LOG("[PWR] ext1 wakeup config failed (%d); stay awake",
            static_cast<int>(wakeErr));
    if (m_display != nullptr) {
      m_display->panelSleep(false);
      m_display->setBrightnessRaw(savedBrightness);
    }
    m_enteringSleep = false;
    m_lastMotionMs = millis();
    return;
  }
#if CFG_AUTO_SLEEP_TIMER_WAKE_MS > 0
  esp_sleep_enable_timer_wakeup(
      static_cast<uint64_t>(CFG_AUTO_SLEEP_TIMER_WAKE_MS) * 1000ULL);
#endif

  CFG_LOG("[PWR] entering deep sleep; move device to wake");
  Serial.flush();
  esp_deep_sleep_start();  // 不返回
}
