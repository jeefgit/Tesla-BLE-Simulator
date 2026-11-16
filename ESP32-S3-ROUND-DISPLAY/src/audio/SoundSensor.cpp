#include "audio/SoundSensor.h"

#include "config.h"

#if CFG_ELF_SOUND_ENABLE

#include <Arduino.h>
#include <Wire.h>
#include <driver/i2s_tdm.h>
#include <esp_err.h>
#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ES7210 + I2S TDM 双麦采集与特征提取（design/ELF-DESIGN.md §5.8–5.10）。
//
// 线程模型：
//   core 1 音频任务：每 16ms 读 256 帧 -> 计算 level/low/onset/beat -> 写快照
//   主任务 read()：只在临界区里拷贝 8 字节快照，随后清掉一次性 beat 标志
// 音频任务里不调用任何 LVGL API。
//
// 寄存器值来自微雪 ES7210 示例的 16kHz / 16bit / 从模式序列；
// 若实机采样恒为 0，先查 AXP2101 ALDO1、MCLK=GPIO42，再微调本文件顶部表。

namespace {

constexpr uint8_t kEs7210Addr = 0x40;
constexpr uint8_t kEs7210RegGain1 = 0x43;
constexpr uint8_t kEs7210RegGain2 = 0x44;

// 每 16ms 读 128 帧（1024B），两半合计 256 帧；分析缓冲 < 2KB。
constexpr size_t kFramesPerRead = 128;
constexpr size_t kReadsPerTick = 2;
constexpr size_t kBlockSamples = kFramesPerRead * 4;
constexpr size_t kBlockBytes = kBlockSamples * sizeof(int16_t);

// 低通截止 180Hz（@16kHz）。
constexpr float kLowCutAlpha = 0.0683f;  // 1 - exp(-2π*180/16000)
constexpr float kDcBlockR = 0.995f;
constexpr float kLevelFloorDb = -60.0f;
constexpr float kLowFloorDb = -50.0f;
// 200ms 电平平滑、1.5s 低频基线。
constexpr float kLevelSmoothAlpha = 0.08f;
constexpr float kBaselineAlpha = 0.011f;
// 基线不为 0，避免静音时 onset 被放大器底噪拉爆。
constexpr float kOnsetEps = 60.0f;
constexpr uint32_t kWarmupMs = 3000UL;
constexpr uint32_t kDiscardTicks = 8;  // 约 130ms，等偏置/高通稳定
constexpr uint32_t kBpmSwitchMs = 1000UL;
constexpr uint32_t kBeginRetryMs = 5000UL;
constexpr uint8_t kMaxBeginAttempts = 5;
constexpr uint16_t kIoiMinMs = 200;   // 与不应期一致，再靠折叠落到 60–200
constexpr uint16_t kIoiMaxMs = 1200;

struct RegValue {
  uint8_t reg;
  uint8_t value;
  const char* note;
};

// 上电初始化（从模式，不写 0x08 主模式位）。对齐 esp-bsp es7210_config_codec
// 的寄存器表；0x01 时钟关闭寄存器故意不写（写错会关掉 ADC3/4 时钟）。
const RegValue kInitTable[] = {
    {0x00, 0xFF, "soft reset"},
    {0x00, 0x32, "reset release"},
    {0x09, 0x30, "time ctrl0"},
    {0x0A, 0x30, "time ctrl1"},
    // ADC1-4 高通：2.4Hz 附近，去掉麦克风偏置。
    {0x23, 0x2A, "HPF adc12_1"},
    {0x22, 0x0A, "HPF adc12_2"},
    {0x21, 0x2A, "HPF adc34_1"},
    {0x20, 0x0A, "HPF adc34_2"},
    // 16kHz、MCLK = 4.096MHz（256 * fs）：adc_div=1，dll=1，doubler=1。
    {0x02, 0xC1, "16k mclk div"},
    {0x07, 0x20, "OSR 16k"},
    {0x04, 0x01, "LRCK div hi"},
    {0x05, 0x00, "LRCK div lo"},
    // 16bit + I2S 协议：0x60；配合 0x12=0x02 走 1xFS TDM。
    {0x11, 0x60, "I2S 16bit"},
    // TDM 输出：0x12[1:0]=10 -> TDM I2S，四路 ADC 走同一根 SDOUT1。
    // 若写 0x00（ADC12->SDOUT1、ADC34->SDOUT2），slot2/3 会是空的；
    // 若日志里 slot0 有声音而 slot2 恒为 0，优先检查这里。
    {0x12, 0x02, "TDM I2S out"},
    // 0xC3 = 微雪旧例程值；新版 esp_codec_dev 用 0x43（差 bit7）。
    // 若实机底噪/失真异常，优先把这一行换成 0x43 再试。
    {0x40, 0xC3, "VDDA 3.3V"},
    {0x41, 0x70, "MIC12 bias 2.87V"},
    {0x42, 0x70, "MIC34 bias 2.87V"},
    // MIC1/2 前置放大器上电（0x08 = on；0x00 会保持关闭）。
    {0x47, 0x08, "MIC1 power"},
    {0x48, 0x08, "MIC2 power"},
    // 先关 DLL，等时钟稳定后在 kStartTable 里再开偏置/ADC/PGA。
    {0x06, 0x04, "DLL power down"},
};

// 启动序列（在写增益之后）：打开 MIC12 偏置/ADC/PGA，并让芯片退出配置态。
const RegValue kStartTable[] = {
    {0x4B, 0x0F, "MIC12 bias/adc/pga on"},
    {0x4C, 0x0F, "MIC34 bias/adc/pga on"},
    {0x00, 0x71, "enable"},
    {0x00, 0x41, "run"},
};

struct Impl {
  portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
  SoundFrame frame;
  i2s_chan_handle_t rx = nullptr;
  TaskHandle_t task = nullptr;
  volatile bool taskRun = false;
  uint32_t generation = 0;
  uint32_t lastBeginAttemptMs = 0;
  uint32_t readFailureCount = 0;
  uint32_t lastReadFailureLogMs = 0;
  uint8_t beginAttempts = 0;
  uint8_t discardTicks = 0;

  // i2s_channel_read 要求缓冲 4 字节对齐。
  alignas(4) int16_t block[kBlockSamples] = {};

  // 分析状态（只被音频任务写，临界区只保护 frame）。
  uint32_t startMs = 0;
  float dcX = 0;
  float dcY = 0;
  float lp1 = 0;
  float lp2 = 0;
  float prevFullSq = 0;
  float prevLowSq = 0;
  uint32_t prevCount = 0;
  float levelSmooth = 0;
  float baseline = 0;

  bool haveOnset = false;
  float onsetPrev = 0;
  float onsetPrev2 = 0;
  float levelPrev = 0;

  uint32_t lastBeatMs = 0;
  uint8_t beatCount = 0;
  uint16_t ioi[8] = {};
  uint8_t ioiPos = 0;
  uint8_t ioiCount = 0;
  float bpm = 0;
  uint8_t confidence = 0;
  float pendingBpm = 0;
  uint32_t pendingSince = 0;
  uint32_t quietSince = 0;
  uint32_t lastNonZeroMs = 0;
  bool zeroLogged = false;

  uint32_t lastLogMs = 0;
  uint32_t lastSlotLogMs = 0;
  bool logBeatSeen = false;
};

Impl g_impl;

bool esWrite(uint8_t reg, uint8_t value, const char* note) {
  Wire.beginTransmission(kEs7210Addr);
  Wire.write(reg);
  Wire.write(value);
  const uint8_t err = Wire.endTransmission();
  CFG_LOG("[AUDIO] es7210 0x%02X=0x%02X %-18s %s", reg, value, note,
          err == 0 ? "ok" : "NACK");
  return err == 0;
}

bool writeTable(const RegValue* table, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    if (!esWrite(table[i].reg, table[i].value, table[i].note)) {
      return false;
    }
    // 软复位后给芯片一点时间，再写后续寄存器。
    if (table[i].reg == 0x00 && table[i].value == 0xFF) delay(10);
  }
  return true;
}

i2s_chan_handle_t initI2s() {
  i2s_chan_config_t chan_cfg =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num = 4;
  chan_cfg.dma_frame_num = 256;
  chan_cfg.auto_clear = true;

  i2s_chan_handle_t rx = nullptr;
  esp_err_t err = i2s_new_channel(&chan_cfg, nullptr, &rx);
  if (err != ESP_OK) {
    CFG_LOG("[AUDIO] i2s_new_channel failed: %s", esp_err_to_name(err));
    return nullptr;
  }

  i2s_tdm_config_t cfg = {};
  cfg.clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(CFG_ELF_SOUND_SAMPLE_RATE);
  cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  cfg.slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(
      I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
      static_cast<i2s_tdm_slot_mask_t>(I2S_TDM_SLOT0 | I2S_TDM_SLOT1 |
                                       I2S_TDM_SLOT2 | I2S_TDM_SLOT3));
  cfg.slot_cfg.total_slot = 4;
  cfg.gpio_cfg.mclk = GPIO_NUM_42;
  cfg.gpio_cfg.bclk = GPIO_NUM_9;
  cfg.gpio_cfg.ws = GPIO_NUM_45;
  cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
  cfg.gpio_cfg.din = GPIO_NUM_10;
  cfg.gpio_cfg.invert_flags.mclk_inv = 0;
  cfg.gpio_cfg.invert_flags.bclk_inv = 0;
  cfg.gpio_cfg.invert_flags.ws_inv = 0;

  err = i2s_channel_init_tdm_mode(rx, &cfg);
  if (err != ESP_OK) {
    CFG_LOG("[AUDIO] i2s tdm init failed: %s", esp_err_to_name(err));
    i2s_del_channel(rx);
    return nullptr;
  }
  return rx;
}

void resetAnalysis(Impl& s) {
  s.dcX = 0;
  s.dcY = 0;
  s.lp1 = 0;
  s.lp2 = 0;
  s.prevFullSq = 0;
  s.prevLowSq = 0;
  s.prevCount = 0;
  s.levelSmooth = 0;
  s.baseline = 0;
  s.haveOnset = false;
  s.onsetPrev = 0;
  s.onsetPrev2 = 0;
  s.levelPrev = 0;
  s.lastBeatMs = 0;
  s.beatCount = 0;
  s.ioiPos = 0;
  s.ioiCount = 0;
  s.bpm = 0;
  s.confidence = 0;
  s.pendingBpm = 0;
  s.pendingSince = 0;
  s.quietSince = 0;
  s.lastNonZeroMs = millis();
  s.zeroLogged = false;
  s.lastLogMs = 0;
  s.lastSlotLogMs = 0;
  s.logBeatSeen = false;
}

float clampf(float v, float lo, float hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

void pushIoi(Impl& s, uint16_t ioiMs) {
  if (ioiMs < kIoiMinMs || ioiMs > kIoiMaxMs) return;
  s.ioi[s.ioiPos] = ioiMs;
  s.ioiPos = static_cast<uint8_t>((s.ioiPos + 1) % 8);
  if (s.ioiCount < 8) ++s.ioiCount;
}

uint16_t medianIoi(const Impl& s) {
  uint16_t v[8];
  const uint8_t n = s.ioiCount;
  for (uint8_t i = 0; i < n; ++i) v[i] = s.ioi[i];
  for (uint8_t i = 1; i < n; ++i) {
    const uint16_t key = v[i];
    int8_t j = static_cast<int8_t>(i - 1);
    while (j >= 0 && v[j] > key) {
      v[j + 1] = v[j];
      --j;
    }
    v[j + 1] = key;
  }
  if (n == 0) return 0;
  if (n % 2 == 1) return v[n / 2];
  return static_cast<uint16_t>((v[n / 2 - 1] + v[n / 2]) / 2);
}

void updateBpm(Impl& s, uint32_t now) {
  // 第二拍（1 个 IOI）就给出初值，否则 60 BPM 慢歌要等第三拍、
  // 超过 ElfOverlay 的 1.2s 统计窗口，永远进不了 Grooving。
  if (s.ioiCount < 1) return;
  const uint16_t med = medianIoi(s);
  if (med == 0) return;
  float raw = 60000.0f / static_cast<float>(med);
  // 折叠到 60–200：240 -> 120，避免把八分音符当主拍。
  while (raw > CFG_ELF_SOUND_BEAT_MAX_BPM) raw *= 0.5f;
  while (raw < CFG_ELF_SOUND_BEAT_MIN_BPM) raw *= 2.0f;

  if (s.bpm <= 0.0f) {
    s.bpm = raw;
    s.pendingBpm = 0;
    return;
  }
  const float diff = fabsf(raw - s.bpm) / s.bpm;
  if (diff < 0.08f) {
    s.bpm += (raw - s.bpm) * 0.25f;
    s.pendingBpm = 0;
    return;
  }
  // 变化较大时先挂起，稳定 1s 再切，避免数字乱跳。
  if (s.pendingBpm <= 0.0f ||
      fabsf(raw - s.pendingBpm) / s.pendingBpm > 0.08f) {
    s.pendingBpm = raw;
    s.pendingSince = now;
  } else if (now - s.pendingSince >= kBpmSwitchMs) {
    s.bpm = raw;
    s.pendingBpm = 0;
  }
}

void updateConfidence(Impl& s) {
  if (s.ioiCount < 3) {
    s.confidence = 60;
    return;
  }
  uint16_t v[3];
  for (uint8_t i = 0; i < 3; ++i) {
    const uint8_t idx =
        static_cast<uint8_t>((s.ioiPos + 8 - 3 + i) % 8);
    v[i] = s.ioi[idx];
  }
  const float mean = (v[0] + v[1] + v[2]) / 3.0f;
  float maxDev = 0;
  for (uint8_t i = 0; i < 3; ++i) {
    const float d = fabsf(static_cast<float>(v[i]) - mean);
    if (d > maxDev) maxDev = d;
  }
  const float ratio = maxDev / (mean > 0.0f ? mean : 1.0f);
  // ±25% 内视为“稳定”，比值越小置信度越高。
  float c;
  if (ratio <= 0.25f) {
    c = 200.0f + 55.0f * (1.0f - ratio / 0.25f);
  } else {
    c = 200.0f * (1.0f - (ratio - 0.25f) / 0.5f);
  }
  s.confidence = static_cast<uint8_t>(clampf(c, 0.0f, 255.0f) + 0.5f);
}

struct TickStats {
  float slotSq[4] = {};
  float fullSq = 0;
  float lowSq = 0;
  uint32_t count = 0;
  bool nonZero = false;
};

void accumulate(Impl& s, const int16_t* buf, size_t frames, TickStats& t) {
  for (size_t i = 0; i < frames; ++i) {
    const int16_t s0 = buf[i * 4 + 0];
    const int16_t s1 = buf[i * 4 + 1];
    const int16_t s2 = buf[i * 4 + 2];
    const int16_t s3 = buf[i * 4 + 3];
    t.slotSq[0] += static_cast<float>(s0) * s0;
    t.slotSq[1] += static_cast<float>(s1) * s1;
    t.slotSq[2] += static_cast<float>(s2) * s2;
    t.slotSq[3] += static_cast<float>(s3) * s3;
    if (s0 != 0 || s1 != 0 || s2 != 0 || s3 != 0) t.nonZero = true;

    // slot0 = MIC1、slot2 = MIC2；两路平均抑制单麦被遮挡。
    const float x = 0.5f * static_cast<float>(s0 + s2);
    const float y = x - s.dcX + kDcBlockR * s.dcY;
    s.dcX = x;
    s.dcY = y;

    s.lp1 += kLowCutAlpha * (y - s.lp1);
    s.lp2 += kLowCutAlpha * (s.lp1 - s.lp2);

    t.fullSq += y * y;
    t.lowSq += s.lp2 * s.lp2;
  }
  t.count += static_cast<uint32_t>(frames);
}

#if CFG_ELF_SOUND_LOG
void logSlots(const TickStats& t, uint32_t now, Impl& s) {
  if (now - s.startMs >= 2000UL) return;
  if (now - s.lastSlotLogMs < 250UL) return;
  s.lastSlotLogMs = now;
  const float inv = 1.0f / static_cast<float>(t.count);
  CFG_LOG("[AUDIO] slot0=%d slot1=%d slot2=%d slot3=%d",
          static_cast<int>(sqrtf(t.slotSq[0] * inv) + 0.5f),
          static_cast<int>(sqrtf(t.slotSq[1] * inv) + 0.5f),
          static_cast<int>(sqrtf(t.slotSq[2] * inv) + 0.5f),
          static_cast<int>(sqrtf(t.slotSq[3] * inv) + 0.5f));
}
#endif

void analyzeTick(Impl& s, const TickStats& t, uint32_t now,
                 uint32_t generation) {
#if CFG_ELF_SOUND_LOG
  logSlots(t, now, s);
#endif

  // 全 0 采样检查（供电/MCLK/寄存器问题），每次 start 只报一次。
  if (t.nonZero) {
    s.lastNonZeroMs = now;
  } else if (!s.zeroLogged && now - s.startMs > 1000UL &&
             now - s.lastNonZeroMs >= 1000UL) {
    s.zeroLogged = true;
    CFG_LOG("[AUDIO] all-zero samples (check ALDO1/MCLK=42/ES7210 regs)");
  }

  // 32ms 滑窗 = 上一 tick + 本 tick（帧移 16ms）。
  const float fullSq = s.prevFullSq + t.fullSq;
  const float lowSq = s.prevLowSq + t.lowSq;
  const uint32_t count = s.prevCount + t.count;
  s.prevFullSq = t.fullSq;
  s.prevLowSq = t.lowSq;
  s.prevCount = t.count;
  if (count == 0) return;

  const float inv = 1.0f / static_cast<float>(count);
  const float fullRms = sqrtf(fullSq * inv);
  const float lowRms = sqrtf(lowSq * inv);

  // 全带 RMS -> dB -> 0..255，再做 200ms 平滑。
  const float db = 20.0f * log10f(fullRms / 32768.0f + 1e-6f);
  const float levelRaw =
      clampf((db - kLevelFloorDb) * (255.0f / -kLevelFloorDb), 0.0f, 255.0f);
  s.levelSmooth += (levelRaw - s.levelSmooth) * kLevelSmoothAlpha;
  const uint8_t level =
      static_cast<uint8_t>(clampf(s.levelSmooth, 0.0f, 255.0f) + 0.5f);

  const float lowDb = 20.0f * log10f(lowRms / 32768.0f + 1e-6f);
  const uint8_t lowOut = static_cast<uint8_t>(
      clampf((lowDb - kLowFloorDb) * (255.0f / -kLowFloorDb), 0.0f, 255.0f) +
      0.5f);

  // 1.5s 慢速基线 + onset。
  s.baseline += (lowRms - s.baseline) * kBaselineAlpha;
  float onset = lowRms / (s.baseline + kOnsetEps) - 1.0f;
  if (onset < 0.0f) onset = 0.0f;

  const bool warmup = (now - s.startMs) < kWarmupMs;
  bool newBeat = false;

  // 延迟 1 帧峰值确认：前一帧大于更前一帧，且不小于当前帧。
  if (!warmup && s.haveOnset &&
      s.onsetPrev > CFG_ELF_SOUND_ONSET_THRESHOLD &&
      s.onsetPrev > s.onsetPrev2 && s.onsetPrev >= onset &&
      s.levelPrev > static_cast<float>(CFG_ELF_SOUND_NOISE_GATE)) {
    const uint32_t beatMs = now - CFG_ELF_SOUND_HOP_MS;
    if (s.lastBeatMs == 0 ||
        beatMs - s.lastBeatMs >= CFG_ELF_SOUND_BEAT_REFRACTORY_MS) {
      newBeat = true;
      const uint32_t ioiMs =
          (s.lastBeatMs == 0) ? 0 : (beatMs - s.lastBeatMs);
      s.lastBeatMs = beatMs;
      ++s.beatCount;
      s.logBeatSeen = true;
      if (ioiMs > 0) pushIoi(s, static_cast<uint16_t>(ioiMs));
      updateBpm(s, now);
      updateConfidence(s);
    }
  }
  s.onsetPrev2 = s.onsetPrev;
  s.onsetPrev = onset;
  s.levelPrev = s.levelSmooth;
  s.haveOnset = true;

  if (s.lastBeatMs != 0 && now - s.lastBeatMs > 2000UL) {
    s.confidence = 0;
  }

  // 静音 2s：BPM 归零，等待下一次律动。
  if (level < CFG_ELF_SOUND_NOISE_GATE) {
    if (s.quietSince == 0) {
      s.quietSince = now;
    } else if (now - s.quietSince >= CFG_ELF_SOUND_QUIET_MS) {
      s.bpm = 0;
      s.pendingBpm = 0;
      s.confidence = 0;
      s.ioiCount = 0;
      s.ioiPos = 0;
      s.lastBeatMs = 0;
    }
  } else {
    s.quietSince = 0;
  }

  SoundFrame f;
  f.level = level;
  f.low = lowOut;
  f.bpm = static_cast<uint8_t>(clampf(s.bpm, 0.0f, 255.0f) + 0.5f);
  f.confidence = s.confidence;
  f.beatCount = s.beatCount;
  f.loud = level >= CFG_ELF_SOUND_NOISE_GATE;

  // 快照发布：只在临界区里写 8 字节，<10µs。
  portENTER_CRITICAL(&s.mux);
  if (generation == s.generation) {
    f.beat = newBeat || s.frame.beat;
    s.frame = f;
  }
  portEXIT_CRITICAL(&s.mux);

#if CFG_ELF_SOUND_LOG
  if (now - s.lastLogMs >= 500UL) {
    s.lastLogMs = now;
    CFG_LOG("[AUDIO] lvl=%u low=%u bpm=%u conf=%u beat=%u",
            static_cast<unsigned>(f.level), static_cast<unsigned>(f.low),
            static_cast<unsigned>(f.bpm), static_cast<unsigned>(f.confidence),
            s.logBeatSeen ? 1U : 0U);
    s.logBeatSeen = false;
  }
#endif
}

void audioTask(void* arg) {
  (void)arg;
  Impl& s = g_impl;
  TickType_t lastWake = xTaskGetTickCount();
  uint32_t generation;
  portENTER_CRITICAL(&s.mux);
  generation = s.generation;
  portEXIT_CRITICAL(&s.mux);

  while (s.taskRun) {
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(CFG_ELF_SOUND_HOP_MS));

    TickStats tick;
    for (size_t i = 0; i < kReadsPerTick; ++i) {
      size_t bytesRead = 0;
      const esp_err_t err = i2s_channel_read(s.rx, s.block, kBlockBytes,
                                             &bytesRead, 30);
      constexpr size_t kFrameBytes = 4 * sizeof(int16_t);
      if (bytesRead > 0 && bytesRead <= kBlockBytes &&
          bytesRead % kFrameBytes == 0) {
        accumulate(s, s.block, bytesRead / kFrameBytes, tick);
      }
      if (err != ESP_OK || bytesRead != kBlockBytes) {
        ++s.readFailureCount;
        const uint32_t now = millis();
        if (now - s.lastReadFailureLogMs >= 5000UL) {
          s.lastReadFailureLogMs = now;
          if (err != ESP_OK) {
            CFG_LOG("[AUDIO] i2s read failed: %s (%u total)",
                    esp_err_to_name(err),
                    static_cast<unsigned>(s.readFailureCount));
          } else {
            CFG_LOG("[AUDIO] i2s short read: %u/%u bytes (%u total)",
                    static_cast<unsigned>(bytesRead),
                    static_cast<unsigned>(kBlockBytes),
                    static_cast<unsigned>(s.readFailureCount));
          }
        }
        break;
      }
    }
    if (tick.count == 0) continue;

    // 丢掉开头 8 帧，等 ES7210 偏置/高通稳定后再发布特征。
    if (s.discardTicks > 0) {
      --s.discardTicks;
      continue;
    }
    analyzeTick(s, tick, millis(), generation);
  }

  s.task = nullptr;
  vTaskDelete(nullptr);
}

}  // namespace

bool SoundSensor::begin() {
  if (m_available) return true;
  Impl& s = g_impl;
  const uint32_t now = millis();
  if (s.beginAttempts >= kMaxBeginAttempts ||
      (s.beginAttempts > 0 &&
       now - s.lastBeginAttemptMs < kBeginRetryMs)) {
    return false;
  }
  s.lastBeginAttemptMs = now;
  ++s.beginAttempts;

  m_impl = &g_impl;

  bool ok = writeTable(kInitTable, sizeof(kInitTable) / sizeof(kInitTable[0]));
  if (ok) {
    delay(10);
    const uint8_t gain =
        static_cast<uint8_t>(0x10 | (CFG_ELF_SOUND_LEVEL_GAIN & 0x0F));
    ok = esWrite(kEs7210RegGain1, gain, "MIC1 gain") &&
         esWrite(kEs7210RegGain2, gain, "MIC2 gain");
  }
  if (ok) {
    ok = writeTable(kStartTable, sizeof(kStartTable) / sizeof(kStartTable[0]));
  }
  if (ok) delay(30);

  i2s_chan_handle_t rx = ok ? initI2s() : nullptr;
  if (rx == nullptr) {
    CFG_LOG("[AUDIO] init failed (attempt %u/%u)",
            static_cast<unsigned>(s.beginAttempts),
            static_cast<unsigned>(kMaxBeginAttempts));
    if (s.beginAttempts == kMaxBeginAttempts) {
      CFG_LOG("[AUDIO] init retry limit reached");
    }
    return false;
  }

  g_impl.rx = rx;
  m_available = true;
  CFG_LOG("[AUDIO] es7210 ready (16k, TDM 4 slots, MCLK=GPIO42)");
  return true;
}

bool SoundSensor::start() {
  if (!m_available || m_impl == nullptr) return false;
  Impl& s = *static_cast<Impl*>(m_impl);
  if (m_running) return true;
  if (s.rx == nullptr) return false;
  // 上次 stop 超时后先关 I2S 并等待旧任务退出，不能并行启动第二个任务。
  if (s.task != nullptr) {
    s.taskRun = false;
    const esp_err_t disableErr = i2s_channel_disable(s.rx);
    if (disableErr != ESP_OK && disableErr != ESP_ERR_INVALID_STATE) {
      CFG_LOG("[AUDIO] i2s disable failed: %s",
              esp_err_to_name(disableErr));
    }
    m_running = false;
    for (int i = 0; i < 40 && s.task != nullptr; ++i) {
      vTaskDelay(pdMS_TO_TICKS(2));
    }
    if (s.task != nullptr) {
      CFG_LOG("[AUDIO] previous task still running");
      return false;
    }
  }

  esp_err_t err = i2s_channel_enable(s.rx);
  if (err != ESP_OK) {
    CFG_LOG("[AUDIO] i2s enable failed: %s", esp_err_to_name(err));
    return false;
  }

  resetAnalysis(s);
  s.startMs = millis();
  s.lastNonZeroMs = s.startMs;
  s.readFailureCount = 0;
  s.lastReadFailureLogMs = s.startMs;
  s.discardTicks = static_cast<uint8_t>(kDiscardTicks);

  portENTER_CRITICAL(&s.mux);
  ++s.generation;
  s.frame = SoundFrame{};
  portEXIT_CRITICAL(&s.mux);

  s.taskRun = true;
  // core 1、优先级 2：大部分时间阻塞在 DMA 读取上，只在 16ms 边界短暂运行。
  const BaseType_t created = xTaskCreatePinnedToCore(
      audioTask, "elf_sound", 4096, nullptr, 2, &s.task, 1);
  if (created != pdPASS) {
    s.taskRun = false;
    s.task = nullptr;
    i2s_channel_disable(s.rx);
    CFG_LOG("[AUDIO] task create failed");
    return false;
  }

  m_running = true;
  return true;
}

void SoundSensor::stop() {
  if (m_impl == nullptr) return;
  Impl& s = *static_cast<Impl*>(m_impl);

  s.taskRun = false;
  if (m_running && s.rx != nullptr) {
    const esp_err_t err = i2s_channel_disable(s.rx);
    if (err != ESP_OK) {
      CFG_LOG("[AUDIO] i2s disable failed: %s", esp_err_to_name(err));
    }
  }
  m_running = false;
  // 先禁用 I2S 释放阻塞 read，再短暂等待任务自删，避免主循环长时间卡住。
  for (int i = 0; i < 40 && s.task != nullptr; ++i) {
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  if (s.task != nullptr) {
    CFG_LOG("[AUDIO] task stop timeout");
  }

  portENTER_CRITICAL(&s.mux);
  ++s.generation;
  s.frame = SoundFrame{};
  portEXIT_CRITICAL(&s.mux);
}

bool SoundSensor::running() const { return m_running; }

SoundFrame SoundSensor::read() {
  if (m_impl == nullptr) return SoundFrame{};
  Impl& s = *static_cast<Impl*>(m_impl);

  SoundFrame out;
  portENTER_CRITICAL(&s.mux);
  out = s.frame;
  s.frame.beat = false;
  portEXIT_CRITICAL(&s.mux);
  return out;
}

bool SoundSensor::available() const { return m_available; }

#else  // !CFG_ELF_SOUND_ENABLE

// 关闭声音功能时提供空实现：不 include I2S 头、不初始化任何音频外设，
// 编译与运行行为完全回到阶段 4。

bool SoundSensor::begin() { return false; }

bool SoundSensor::start() { return false; }

void SoundSensor::stop() {}

bool SoundSensor::running() const { return false; }

SoundFrame SoundSensor::read() { return SoundFrame{}; }

bool SoundSensor::available() const { return false; }

#endif  // CFG_ELF_SOUND_ENABLE
