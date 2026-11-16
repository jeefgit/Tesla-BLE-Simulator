#pragma once

#include <stdint.h>

// 音频线程发布的最新一帧特征（只读快照，不含任何原始采样）。
struct SoundFrame {
  uint8_t level = 0;      // 0-255 对数音量（已做噪声门/平滑）
  uint8_t low = 0;        // 0-255 低频能量（40-180Hz），用于节拍
  uint8_t bpm = 0;        // 0 = 未知，60-200
  uint8_t confidence = 0; // 0-255 BPM 置信度
  uint8_t beatCount = 0;  // 累计拍数
  bool beat = false;      // 自上次 read() 起出现过新拍（读取即清）
  bool loud = false;      // 是否超过噪声门
};

class SoundSensor {
 public:
  // 初始化 I2C 上的 ES7210 + I2S RX 通道；失败返回 false（可安全重复调用）。
  bool begin();
  // 小精灵显示时启动采集：使能 I2S、启动分析任务。失败返回 false。
  bool start();
  // 小精灵隐藏时停止采集：停任务、禁用 I2S。可安全重复调用。
  void stop();
  bool running() const;
  // 主线程读取最新快照；返回后清掉一次性 beat 标志。
  SoundFrame read();
  bool available() const;

 private:
  // 实现细节全部放在 .cpp 的匿名命名空间，头文件不引入 IDF/Arduino。
  void* m_impl = nullptr;
  bool m_available = false;
  volatile bool m_running = false;
};
