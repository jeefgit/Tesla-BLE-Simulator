# 圆形仪表界面设计稿 v0.1

目标硬件：**Waveshare ESP32-S3-Touch-AMOLED-1.75-B**（466×466，CO5300 QSPI，CST9217 触摸）。
设计参考 `TESLA-DASHBOARD` 的界面语言与数据分层，但针对圆屏重新排版。

- 可交互查看：`design/dashboard-ui.html`（浏览器直接打开，1:1 尺寸，可勾选安全区）
- 本文档：布局坐标、颜色/字号 token、状态机、数据映射与实现注意点

设计稿包含四个状态：

| 状态 | 触发条件 | 页面策略 |
| --- | --- | --- |
| 行驶 DRIVING | `LinkState::Ready` 且握手完成，`charging == false` | 速度环 + 车速 + 档位 + 功率 + 电量/续航 |
| 充电 CHARGING | 同上且 `charging == true` | 速度环切换为 SOC 环，中心显示充电功率 |
| 断连/扫描 LINK STATE | 未 Ready 或握手未完成 | 整屏状态页（SCANNING / CONNECTING / SYNCING / RECONNECTING / ERROR） |
| 配钥 PAIRING | `PairingHint::phase != Idle` 且链路 Ready | 配钥提示优先于断连页 |

---

## 1. 设计原则

1. **圆屏不是一个裁掉角的矩形。** 所有信息放在 R216 安全圆内；顶部状态带收窄到 240px，避免被圆形边缘切掉。
2. **一眼只读一个数。** 行驶时视觉中心是车速，充电时是功率；其余信息降级为 8–12px 的辅助文字。
3. **沿用参考固件的语言。** 纯黑底、Tesla 红 #E82127、绿色表示能量回收/充电、英文 ASCII 标签、断连降级 `--`。
4. **只用可实现的字段。** 不显示设备没有的时间、累计充电量等数据；充电 ETA 由 SOC/功率估算。
5. **静态对象树 + 变化才写。** 与 `TESLA-DASHBOARD/src/app/Dashboard.cpp` 一致，避免高频 `set_text` 造成整屏重绘。

---

## 2. 画布与几何

| 项 | 值 | 说明 |
| --- | --- | --- |
| 逻辑画布 | 466 × 466 | 与面板原生分辨率一致，不做旋转 |
| 可见圆 | R233 | 屏幕物理边界 |
| 安全内容圆 | **R216** | 内容最低点 y≤426，左右最远 x=58..408 |
| 速度/SOC 环 | R170，线宽 6 | 角度 204° → 336°（132°，顶部弧） |
| 环端点标签 | R191 | `0 / 240`（行驶）或 `0 / 100`（充电） |
| 刻度 | R178–189，13 根 | 每 11°，每 3 根为主刻度 |
| 顶部状态带 | x113–353，y44–62 | 三栏：品牌 / 链路 / 锁 |
| 数值区 | y88–246 | 车速 142px |
| 档位 | y260 | P R N D，间距 26px |
| 功率块 | y292–336 | 300px 宽，中心零位 |
| 信息卡 | y344–400 | 132×56 两张，间距 12px |
| 温度行 | y414–426 | 圆底部最窄处，只放两项 |

> 半径换算：圆心 (233,233)。例如 y=414 时圆的半宽为 `sqrt(233² − 181²) ≈ 147px`，所以底部只能放 132px 卡片和一行短文字。

---

## 3. 颜色 Token

| Token | HEX | 用途 |
| --- | --- | --- |
| SCREEN | `#020406` | 圆屏最外圈 / 纯黑底 |
| BG_TOP | `#101923` | 背景中心微亮，向边缘过渡到黑 |
| TEXT | `#F5F7FA` | 主要数值、当前档位 |
| DIM | `#8A93A6` | 单位、副标题、未激活文字 |
| FAINT | `#5A6377` | 区域标签、刻度标签 |
| TRACK | `#1D2733` | 圆环轨道、进度条底槽 |
| LINE | `#1B2430` | 卡片描边、分隔线 |
| TESLA_RED | `#E82127` | 速度环渐变终点、当前档位红点、超速/告警 |
| REGEN_GREEN | `#21C15E` | 能量回收、充电环、充电功率 |
| CAUTION | `#FFB340` | 扫描/连接中、配钥、低电量提醒 |
| ALERT | `#FF453A` | 断连失败、胎压异常、车门开启 |

速度环渐变：`#F4F7FA → #E82127`（沿环从左到右）；充电环渐变：`#8AF0B4 → #21C15E`。
背景使用一层极淡的径向强调光（行驶红 / 充电绿 / 断连琥珀，透明度 7%–15%），AMOLED 上不会造成漏光。

---

## 4. 字体

参考固件只用 Montserrat（ASCII）。本稿用 Bahnschrift 近似显示，落地时建议：

| 层级 | 设计字号 | 字重 | LVGL 建议 |
| --- | --- | --- | --- |
| 车速 | 142 | Light | Montserrat 140，仅生成数字 + `/`，4bpp |
| 充电功率 | 84 | Light | 复用速度字体，缩放或单独 80px |
| 状态标题 | 46 | Light | Montserrat 46 |
| 数值 | 23 | Medium | Montserrat 24 |
| 单位/标签 | 8–10 | Semibold | Montserrat 10 + 字距 1–3px |
| 配钥倒计时 | 96 | Light | 复用 140px 字体或单独 96px |

中文字体不进入固件：界面文案保持英文；串口日志继续用中文。大字字体按需只保留 `0-9 . % - : / k W m h`，避免整字库占用 Flash。

---

## 5. 状态与刷新逻辑

```
SourceStatus.link / encrypted / PairingHint
        │
        ├─ phase != Idle && link == Ready  → 配钥页（最高优先级）
        ├─ link != Ready 或 !encrypted     → LINK STATE 页
        └─ 正常
             ├─ charging == true           → 充电页
             └─ charging == false          → 行驶页
```

- 状态切换只在 `link / encrypted / pairing.phase / charging` 变化时重建可见性，不重建对象树。
- 车速做 200ms ease-out 补间（参考 `Dashboard::onTimer`），数字每 50ms 最多更新一次。
- 数据超过 `CFG_DATA_STALE_MS`（建议 3000ms）：对应数值变 `--`，环归零；链路仍 Ready 时不显示断连页。
- 告警：参考固件用底部 16px ticker，圆屏没有这条边。建议下一版用**圆环呼吸 + 中心一行 10px 文案**，避免遮挡车速。

### 状态页文案（ASCII）

| LinkState | 标题 | 副标题 | 强调色 |
| --- | --- | --- | --- |
| Idle | BLE IDLE | STACK NOT STARTED | FAINT |
| Scanning | SCANNING | SEARCHING FOR TESLA BROADCAST | CAUTION |
| Connecting | CONNECTING | OPENING BLE LINK TO VEHICLE | CAUTION |
| Ready（未握手） | SYNCING | WAITING FOR VEHICLE DATA | TEXT |
| Reconnecting | RECONNECTING | LINK LOST — RETRYING | ALERT |
| Error | LINK ERROR | UNRECOVERABLE LINK FAILURE | ALERT |

底部固定提示：`MAKE SURE THE CAR IS AWAKE`；第二行显示 `VIN …` 与 `ELAPSED Ns`，方便现场确认扫的是哪台车。

---

## 6. 数据映射

直接消费 `TeslaBleBackend` 的 `VehicleState` / `SourceStatus`：

| 区域 | 字段 | 显示规则 |
| --- | --- | --- |
| 车速 | `speedKmh` | 0–240 映射 132°；超量程截断；无效 `--` |
| 档位 | `gear` | 当前档白色 + 红点，其余 `#39434F` |
| 功率 | `motorPowerKw` | 正值白→红向右；负值绿色向左；量程 ±250 kW |
| 电量 | `soc` | 0–100%；≤15% 红色并 1Hz 闪烁 |
| 续航 | `rangeKm` | 整数 km |
| 充电 | `charging` / `chargePowerKw` | 环显示 SOC；中心显示 kW；ETA = (80%−SOC)×75kWh ÷ kW |
| 温度 | `insideTempC` / `outsideTempC` | 默认整数；有小数时 10px |
| 锁 | `locked` | `LOCK` / `UNLOCK`（UNLOCK 用 CAUTION） |
| 链路 | `SourceStatus.link` | 绿=LINK，琥珀=扫描/连接，灰=Idle，红=Error |
| 配钥 | `PairingHint` | `TAP CARD / CONFIRM / TIMEOUT / KEY ADDED / FAILED` |
| 车门/胎压 | `doors[6]` / `tpms[4]` | 本稿未常驻显示，见待确认问题 |

---

## 7. LVGL 落地建议

1. **对象树只建一次**：`buildDriving()` / `buildCharging()` / `buildLinkState()` / `buildPairing()` 按状态切换显隐。
2. **绘制顺序**：背景 → 环（track/progress）→ 文本 → 卡片 → 遮罩/状态页。
3. **圆环**：用 `lv_arc`（`LV_ARC_MODE_NORMAL`，背景弧透明、旋转 204°，范围 0–1320）或两个 `lv_obj` + `lv_draw_arc` 自绘；进度变化仅在整数变化时写。
4. **字体**：`lv_font_conv` 生成仅含所需字形的 4bpp Montserrat；140px 字体约 40–80KB，可放 Flash 而非 PSRAM。
5. **缓冲**：466×466 RGB565 全屏约 425KB；建议双 1/4 缓冲（各 ~106KB）放内部 RAM，或单缓冲 + PSRAM。QSPI 刷新优先验证 30fps 下的撕裂。
6. **触摸**：CST9217 坐标需按面板实际方向映射；圆屏建议只做「点按/长按」两类手势，避免滑动与 UI 冲突。
7. **烧屏**：AMOLED 长时间静止会留影。建议亮度 60% 以下，并让环/小元素做 ±1px 缓慢位移或 5 分钟微调。

---

## 8. 待确认

1. 顶部左侧放 **TESLA 字标** 是否合适？如果板子后续加 RTC/SNTP，可换回时间。
2. 车门/胎压是否需要常驻？当前圆屏空间不足，建议只在异常时用环呼吸 + 一行短提示。
3. 功率量程用 ±250 kW 还是 ±400 kW（Plaid/性能版）？
4. 充电页的 75 kWh 电池容量假设是否按具体车型调整？
5. 是否需要超速红环、低电量呼吸等动效？动效会增加 LVGL 重绘面积。
