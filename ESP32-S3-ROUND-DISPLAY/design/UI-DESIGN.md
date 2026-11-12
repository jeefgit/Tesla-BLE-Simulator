# 圆形仪表界面设计稿 v0.2

目标硬件：**Waveshare ESP32-S3-Touch-AMOLED-1.75-B**（466×466，CO5300 QSPI，CST9217 触摸）。
本版按 `src/ui/LvglDashboard.cpp` 的实机实现重新校对，重点同步了**行驶页**的布局、
配色与字号；其余三个状态页仍沿用 v0.1 版式，后续按同一方法校对。

- 可交互查看：`design/dashboard-ui.html`（浏览器直接打开，1:1 尺寸，可勾选安全区）
- 本文档：布局坐标、颜色/字号 token、状态机、数据映射与实现注意点
- 预览图：`design/preview-driving.png` 已按当前固件重新生成

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

> 下表坐标均取自 `LvglDashboard.cpp` 的 `lv_obj_set_pos/size`，单位为屏幕像素。

| 项 | 值 | 说明 |
| --- | --- | --- |
| 逻辑画布 | 466 × 466 | 与面板原生分辨率一致，不做旋转 |
| 可见圆 | R233 | 屏幕物理边界 |
| 速度 / SOC 环 | R170，线宽 6 | 控件 340×340 @ (63,63)，角度 204° → 336°（132°） |
| 环端点标签 | x62 / x404，y150 | Montserrat 10，`0 / 240`（行驶）或 `0 / 100`（充电） |
| 刻度 | 无 | 当前固件不绘制刻度，保持车速大字周围干净 |
| 顶部状态带 | 240×20 @ (113,43) | 三栏：品牌 14px / 链路 14px / 锁 14px |
| 车速数值 | 中心 (233,155) | Montserrat 48 + `transform_scale=600`，实测约 112px |
| 单位 KM/H | y218，高 18 | Montserrat 14，青色 |
| 档位胶囊 | 168×40 @ (149,248) | 深色底 + 36×34 青色高亮块，档位字 20px |
| 功率面板 | 300×52 @ (83,298) | 内含 268×5 功率条 @ (98,335)，中心零位 |
| 信息面板 | 300×52 @ (83,359) | 电量 / 续航左右分栏，分隔线 x150 |
| 温度条 | 240×32 @ (113,418) | 圆底最窄处，只放 OUTSIDE / CABIN 两项 |

> **圆屏裁切提醒**：底部温度面板下沿到 y450，左右两端在 y>430 的区域会超出可见圆。
> 当前实现依赖 `overflow:hidden` 把它裁成弧形；R216 只作为文字安全参考，内容实际更靠近边缘。


---

## 3. 颜色 Token

| Token | HEX | 用途 |
| --- | --- | --- |
| SCREEN | `#020406` | 圆屏最外圈 / 纯黑底 |
| BG_TOP | `#101923` | 设计稿背景中心微亮（固件当前为纯黑） |
| TEXT | `#F5F7FA` | 主要数值、当前档位 |
| DIM | `#8A93A6` | 单位、副标题、未激活文字 |
| FAINT | `#5A6377` | 区域标签、未选档位 |
| TRACK | `#1D2733` | 圆环轨道、进度条底槽 |
| CARD | `#0C1218` | 功率 / 信息 / 温度面板底 |
| CARD_LINE | `#283642` | 功率、信息面板描边 |
| CARD_LINE_SOFT | `#1D2733` | 温度条描边；内部深槽 `#17232C`，分隔线 `#25313A` |
| GAUGE_CYAN | `#43D9F5` | 速度环低段、`KM/H`、电量数值与进度条、档位高亮 |
| TESLA_RED | `#E82127` | 速度环高段、正功率条、低电量/告警 |
| REGEN_GREEN | `#21C15E` | 速度环中段、能量回收、充电环/功率 |
| CAUTION | `#FFB340` | 速度环中高段、续航数值、扫描/连接/配钥 |
| ALERT | `#FF453A` | 断连失败、胎压异常、车门开启 |

**速度环配色不是沿弧渐变，而是整段单色动态插值**：按车速在
`#43D9F5 → #21C15E → #FFB340 → #E82127` 之间做三段线性混合，
节点约在 0 / 80 / 160 / 240 km/h；128 km/h 示例约为 `#A6B94C`。
充电环为纯 `#21C15E`；配钥页环为 `#FFB340`。

背景在固件中是 `#020406` 纯色；设计稿里的径向微光仅用于概念展示，
若要落地需要在 LVGL 里额外绘制一层渐变，当前未实现。


---

## 4. 字体

固件只使用 Montserrat（ASCII）。当前实现全部直接引用 LVGL 内置字号，再用
`transform_scale` 放大标题，避免为每个状态单独生成大字号字库：

| 层级 | 设计字号 | 字重 | LVGL 实现 |
| --- | --- | --- | --- |
| 车速 | ≈112 | Light | `lv_font_montserrat_48` + `transform_scale=600` |
| 充电功率 | 48 | Light | `lv_font_montserrat_48`（未再放大） |
| 状态标题 | 49–56 | Light | `montserrat_48` + scale 260/300（配钥倒计时 440） |
| 电量 / 续航 | 24 | Medium | `lv_font_montserrat_24` |
| 功率 / 温度 / 档位 | 20 | Medium | `lv_font_montserrat_20` |
| 顶部状态 / KM/H | 14 | Medium | `lv_font_montserrat_14` |
| 标签 / 子文案 | 10 | Semibold | `lv_font_montserrat_10` |

> 早期稿写的是 142 / 84 / 46px 专用字体，但当前固件并未生成这些字号。
> 后续若生成专用大字号，需要同步修改 `lv_conf.h` 与字体转换脚本。

中文字体不进入固件：界面文案保持英文；串口日志继续用中文。

---

## 5. 状态与刷新逻辑

```
SourceStatus.link / PairingHint
        │
        ├─ phase != Idle && link == Ready  → 配钥页（最高优先级）
        ├─ link != Ready                   → LINK STATE 页
        └─ 正常
             ├─ charging == true           → 充电页
             └─ charging == false          → 行驶页
```

- 状态切换只在 `link / pairing.phase / charging` 变化时切换对象显隐，不重建对象树。
- 车速在 `DashboardApp::loop()` 里做 **30ms smoothstep 补间**（`kSpeedTransitionMs = 30`），
  主循环最多每 50ms 渲染一次；车速文字与圆弧共用同一个平滑值。
- 圆弧按取整后的 km/h 写入 `lv_arc`（0–1000），只有可见数字变化时才触发重绘。
- 数据过期降级 `CFG_DATA_STALE_MS` 目前只是设计建议，固件尚未实现；现场断连时会直接进入状态页。
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
| 车速 | `speedKmh` | 0–240 映射 132°；超量程截断；无效 `--`，环归零 |
| 速度环色 | `speedKmh` | 青色 → 绿色 → 琥珀 → 红色三段线性插值（80/160 km/h 节点） |
| 档位 | `gear` | 当前档 36×34 青色高亮 + 深色字，其余 `#5A6370`；未知档无高亮 |
| 功率 | `motorPowerKw` | 正值红色向右；负值绿色向左；量程 ±250 kW，比例 0.536 px/kW |
| 电量 | `soc` | 0–100%；≤15% 数值与进度条转红色（当前实机无闪烁） |
| 续航 | `rangeKm` | 整数 km，琥珀色 |
| 充电 | `charging` / `chargePowerKw` | 环显示 SOC；中心显示 kW；ETA = (80%−SOC)×75kWh ÷ kW |
| 温度 | `insideTempC` / `outsideTempC` | 四舍五入整数 °C；标签 OUTSIDE / CABIN |
| 锁 | `locked` | `LOCK` / `UNLOCK`（UNLOCK 用 CAUTION） |
| 链路 | `SourceStatus.link` | 绿=LINK，琥珀=扫描/连接/同步，灰=Idle，红=Retry/Error |
| 配钥 | `PairingHint` | `TAP CARD / CONFIRM / RETRY / KEY ADDED / FAILED` |
| 车门/胎压 | `doors[6]` / `tpms[4]` | 本稿未常驻显示，见待确认问题 |

---

## 7. LVGL 落地建议

1. **对象树只建一次**：`buildDriving()` / `buildCharging()` / `buildCards()` / `buildLinkPage()` / `buildPairingPage()` 按状态切换显隐。
2. **绘制顺序**：背景 → 环（track/progress）→ 文本 → 面板 → 遮罩/状态页。
3. **圆环**：用 `lv_arc`（`LV_ARC_MODE_NORMAL`，旋转 204°，范围 0–1000）实现；进度变化仅在整数变化时写。
4. **字体**：当前复用了 LVGL 内置 Montserrat 14/20/24/48，大标题通过 `transform_scale` 放大；若要更锐利的车速大字，再用 `lv_font_conv` 生成专用 4bpp 字体。
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
