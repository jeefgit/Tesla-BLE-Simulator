# 阶段 5「声音律动」遗留问题清单

> 生成时间：2026-11-16
> 来源：阶段 5 代码整合后的主 agent 静态审查
> 目标：交给另一个 agent 继续处理。**先读本文再动手**，不要回滚已有实现。
> 相关设计：`design/ELF-DESIGN.md`（阶段 5 章节）

---

## 0. 当前状态

- 阶段 5 代码已全部写入工作区，**未提交 git**。
- **未编译、未烧录**（工程约定：不要运行 `pio run`）。
- `design/ELF-DESIGN.md` 已更新：5.1–5.4、5.6 打勾；**5.0（麦克风点亮）、5.5（调参与回归）待实机**。
- 本次审查未发现明确的编译级错误（宏、I2S TDM API、头文件声明均已核对）。
- 已知存在一个**尚未接线**的成员：`include/ui/ElfOverlay.h` 中的 `float m_soundYOffset = 0.0f;`，见 P1-1。

### 涉及文件

| 文件 | 说明 |
| --- | --- |
| `include/config.h` | §5.6 `CFG_ELF_SOUND_*` 配置块 |
| `include/audio/SoundSensor.h` / `src/audio/SoundSensor.cpp` | ES7210 + I2S TDM、特征提取、音频任务 |
| `include/ui/ElfOverlay.h` / `src/ui/ElfOverlay.cpp` | 声音子状态、律动动画、呼吸互斥 |
| `include/app/DashboardApp.h` / `src/app/DashboardApp.cpp` | 生命周期/可见性接线 |
| `design/ELF-DESIGN.md` | 设计文档与清单 |

### 不可违反的约束

1. **不要自动编译**、不要 `pio run`、不要烧录；改完只说明改了哪些文件、需要验证什么。
2. **不要修改 `platformio.ini`**、`include/lv_conf.h`。
3. 不新增第三方依赖（不用 `esp_codec_dev` / `XPowersLib`）。
4. 阶段 5 仍保持：**只收不放**、**不新增 LVGL 对象**、**第一版无 FFT**。
5. `CFG_ELF_SOUND_ENABLE=0` 时，编译与运行行为必须**完全等同阶段 4**。
6. 与用户已有改动冲突时先确认，不要擅自回滚 `design/ELF-DESIGN.md`。

---

## P0：必须实机确认（agent 只能改代码/参数，验收需用户硬件）

这些是「无法通过改代码直接确认」的项，agent 可做的是：保留日志、把可疑点做成易调参数、在代码注释/文档中写明排查顺序。

- [ ] **ES7210 是否出数据**
  - 把 `CFG_ELF_SOUND_LOG` 设为 1，观察启动 2s 内 `[AUDIO] slot0..3` 的 RMS。
  - 全 0 → 先查 AXP2101 **ALDO1 (A3V3)** 是否给麦供电（当前工程无 AXP2101 驱动）、`MCLK=GPIO42`。
  - NACK → 查 `Wire` 15/14 上 `0x40` 与触摸 `0x5A` / IMU `0x6B` / TCA9554 `0x20` 是否冲突。
- [ ] **ES7210 `0x40` 取值**：当前 `0xC3`（微雪旧例程）；若底噪大/失真，改 `0x43`（新版 esp_codec_dev）再试。
- [ ] **TDM 槽序**：确认 `slot0 = MIC1`、`slot2 = MIC2`。
  - 若 slot0 有声音而 slot2 恒 0：优先检查 ES7210 `0x12`（当前 `0x02`）与 `0x47/0x48`。
- [ ] **增益 / 门限标定**：`CFG_ELF_SOUND_LEVEL_GAIN=4`（12dB）、`CFG_ELF_SOUND_NOISE_GATE=10`、onset 阈值，均需按真实环境调整。
- [ ] **BPM / 拍点参数**：用 60/90/120/150 BPM 节拍器验证 `bpm`、`beat`；再测拍手、音乐、静音、环境噪声。
- [ ] **性能回归**：律动时无 LVGL 堆报错、无掉帧、BLE 轮询正常、无看门狗、无 `StoreProhibited`。
- [ ] **`CFG_ELF_SOUND_ENABLE=0` 回归**：编译 + 运行完全回到阶段 4（目前仅静态检查 `#if`）。

---

## P1：代码层面待改进（本次重点）

### P1-1 进入 Grooving 时 `body.y` 瞬跳

- 位置：`src/ui/ElfOverlay.cpp`
  - `ElfOverlay::enterGroove()`（约 1052–1076 行）
  - `ElfOverlay::applySoundVisuals()` 的 `grooveOwns` 分支（约 1330–1374 行）
- 现象：`stopBreathing()` 内部 `lv_obj_set_y(m_body, 0)`，而呼吸在 `+22 ↔ −22` 之间；进入律动时 `grooveW` 从 0 开始，等效于最多约 **44px 的瞬时跳变**。
- 已做的准备：`include/ui/ElfOverlay.h` 已加入 `float m_soundYOffset = 0.0f;`，**尚未使用**。
- 建议实现（不改结构、不加动画对象）：
  1. `enterGroove()` 里不再直接 `stopBreathing()`，交给 `applySoundVisuals()` 在同一 tick 统一接管。
  2. 在 `applySoundVisuals()` 首次发现 `!m_soundBreathStopped` 时：
     - `m_soundYOffset = lv_obj_get_y(m_body);`
     - `stopBreathing(); m_soundBreathStopped = true; m_lastBodyY = -10000;`
  3. 每次 tick 对 `m_soundYOffset` 做指数衰减（时间常数约 90–100ms，`expf(-dt/90)`，< 0.5px 归零）。
  4. `wantY = 弹跳值 + lroundf(m_soundYOffset)`，再走现有 `m_lastBodyY` 缓存写入。
  5. `resetSoundState()`、`resetSoundVisuals()`、`applySoundVisuals()` 的 `!allowed` 分支里把 `m_soundYOffset = 0.0f`。
- 验收：进入律动时眼睛/身体无可见跳动；退出律动仍由 `resumeBreathing()` 350ms 平滑回 `+22`。

### P1-2 触摸让位期间身体 y 冻结

- 位置：`src/ui/ElfOverlay.cpp` `applySoundVisuals()` 的 `yield` 分支（约 1366–1374 行）。
- 现象：`m_soundYieldUntil` 期间呼吸已停、声音层又跳过 y 写入，身体停在最后一帧弹跳位置（可到 −12px），松手后突然继续。
- 建议：让位期间让 `m_soundYOffset` 继续衰减（并把弹跳 `m_pulse` 也按 `dt` 衰减），或在 `yield` 分支里把 y 平滑回 0；触摸仍保持最高优先级（不写 `m_body.x`、不写虹膜）。
- 验收：按住小精灵 1s 再松手，身体无冻结/突跳，600ms 后律动自然恢复。

### P1-3 `SoundSensor::stop()` 最坏阻塞约 300ms

- 位置：`src/audio/SoundSensor.cpp` `SoundSensor::stop()`（约 614–648 行）。
- 现象：200ms 等任务 + 100ms 收尾，都在 `DashboardApp::loop()` 主任务里 `vTaskDelay`；偶发会让 LVGL/BLE 卡一下。
- 建议（二选一，保持简单）：
  - 缩短第一段等待（例如 40×2ms），并在超时后直接 `i2s_channel_disable()` 让阻塞 read 立刻返回；
  - 或把「停止音频」放到独立的一次性收尾流程，`loop()` 只发起、不等待（不要新增长期任务）。
- 验收：反复呼出/隐藏小精灵 20 次，隐藏动画无卡顿，下一次呼出仍能起声音。

### P1-4 音频任务退出/重启竞态

- 位置：`src/audio/SoundSensor.cpp` `start()`（约 572–612 行）、`audioTask()`（约 502–534 行）。
- 现象：
  - `start()` 发现 `s.task != nullptr` 直接返回 false，而 `DashboardApp` 的 `m_soundStartFailed` 要等隐藏再显示才清零 → 本次呼出可能一直没声音。
  - `audioTask()` 末尾 `s.task = nullptr; vTaskDelete(nullptr);` 与 `start()` 的检查存在极窄窗口。
- 建议：`start()` 在发现残留任务时，先 `i2s_channel_disable()` 再短暂等待（复用 P1-3 的短等待），确认退出后再创建；仍失败则允许下一次可见性上升沿重试（例如把 `m_soundStartFailed` 改为「本次可见期间最多重试 N 次」）。
- 验收：快速开关小精灵（1s 内多次），声音能稳定恢复，不出现双任务/日志 "previous task still running" 持续刷。

### P1-5 快歌连眨过密

- 位置：`src/ui/ElfOverlay.cpp` `handleSoundBeat()`（约 1022–1050 行）。
- 现象：`bpm > 150` 时每一拍都 `m_forceDoubleBlink = true`，眨眼显得抽搐。
- 建议：改成「连眨冷却」——记录上次强制连眨时间，例如 1.2s 内最多一次；或只在 `m_beatInGroove % 4 == 0` 时允许。
- 验收：150 BPM 以上音乐，眨眼节奏自然，不连成一片。

### P1-6 `confidence` 未参与判定

- 位置：`src/ui/ElfOverlay.cpp` `updateSoundState()`（约 1192–1262 行）；`m_confidence` 仅存值未用。
- 现象：Grooving 只看 `m_beatsInWindow >= 2` 且 BPM 在 60–200，环境噪声/拍手可能误进律动。
- 建议：在进入 Grooving 的条件里加上 `m_confidence >= 阈值`（例如 80/255，写成宏或常量），或至少用它压制「BPM 刚出现但极不稳定」的情况。注意 `SoundSensor` 在 `ioiCount < 3` 时固定给 60，阈值要兼容。
- 验收：静音/噪声下不误进 Grooving；稳定节拍器仍能进。

### P1-7 `begin()` 的 `static attempted` 不可重试

- 位置：`src/audio/SoundSensor.cpp` `SoundSensor::begin()`（约 538–570 行）。
- 现象：首次 ES7210 初始化失败后，本次开机永久不重试。若 ES7210 上电晚于 MCU（供电时序）则只能重启。
- 建议：保留「不反复刷日志」，但允许间隔重试（例如失败后记时间戳，≥5s 才允许再试一次，最多 N 次），或在 `DashboardApp` 里明确「本次开机不再重试」的行为与日志。
- 验收：模拟首次失败（可临时改 0x40）后不刷屏；真实成功后能恢复。

### P1-8 隐藏后旧任务可能补写一帧

- 位置：`src/audio/SoundSensor.cpp` `SoundSensor::stop()` 末尾与 `analyzeTick()` 发布快照。
- 现象：`stop()` 清空 frame 后，未退出的任务理论上还能再写一帧；因 `ElfOverlay::setSoundFrame()` 在 Hidden/Hiding 直接 return，无视觉影响。
- 建议：低优先级。可在 `SoundSensor::read()`/`stop()` 后加 `m_running` 判断或 generation 计数；若 P1-3/P1-4 修好可忽略。

### P1-9 I2S 读超时与 16ms 节拍

- 位置：`src/audio/SoundSensor.cpp` `audioTask()`（`i2s_channel_read(..., 30)`）。
- 现象：偶发 30ms 超时会丢弃一个 tick，使 onset/BPM 抖动。
- 建议：超时时不要直接 `continue` 丢弃整块，可累计统计并打日志（限频）；确认 DMA `dma_desc_num=4 × dma_frame_num=256` 足够。

---

## P2：文档 / 注释小瑕疵

- [ ] `design/ELF-DESIGN.md` §5.2 写的是 `applySound()`，实现名为 `applySoundVisuals()`，统一命名。
- [ ] `include/ui/ElfOverlay.h` 顶部注释仍写「阶段 4」，补一句阶段 5 声音层。
- [ ] `design/ELF-DESIGN.md` 阶段 5 清单中 **5.0 / 5.5 保持未勾**（等实机），不要提前打勾。
- [ ] 确认 `CFG_ELF_SOUND_*` 在 `config.h` 与文档 §5.6 完全一致（当前一致，改参数时两处同步）。

---

## P3：交付状态（不要替用户做）

- [ ] 不运行 `pio run`、不烧录。
- [ ] 不提交 git（用户自行提交）。
- [ ] 改完输出：改动的文件清单 + 需要用户验证的步骤（对应上面 P0/P1 的验收条件）。

---

## 建议处理顺序（给下一个 agent）

1. **P1-1 + P1-2**：视觉平滑，纯 UI，风险低、收益最明显。
2. **P1-3 + P1-4**：音频任务生命周期，避免卡顿和「第二次没声音」。
3. **P1-5 + P1-6**：律动观感与误触发。
4. **P1-7 ~ P1-9**：健壮性收尾。
5. **P2**：文档同步。
6. 所有参数类改动后，提醒用户做 P0 实机验证（尤其 `CFG_ELF_SOUND_LOG=1` 的启动日志）。
