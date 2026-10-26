# 特斯拉 ESP32 2.8" 仪表盘 · 界面设计规范 v1.0

> 可视化原型：`dashboard/ui-mockup.html`（浏览器打开，可切换 7 种状态、2:1/1:1 缩放）
> 数据源：`Tesla-BLE-Simulator` 的 **SimPayload（53 字节）**，与 `ble_adapter.cpp` 结构体一一对应

---

## 1. 硬件与渲染前提

| 项 | 选型 |
| :--- | :--- |
| 主控 | ESP32-S3（NimBLE 收包 + LVGL 渲染） |
| 屏幕 | 2.8" TFT **320×240 横屏**，ILI9341 / ST7789，SPI |
| 色深 | RGB565 |
| UI 框架 | **LVGL 9.x** |
| 数据节拍 | 800ms（模拟真实 DriveState 刷新周期），UI 层做补间 |
| 字符集 | 全 ASCII（数字 + 英文标签），不带 CJK 点阵，省 Flash |

---

## 2. 数据字段 → 界面元素映射

| SimPayload 字段 | 类型 | 界面元素 | 位置 | 处理规则 |
| :--- | :--- | :--- | :--- | :--- |
| `speed_kmh` | float | **车速大字 74px** | 中区 | 200ms 插值补间；断连显示 `--` |
| `motor_power_kw` | float | **零点功率计**（竖条） | 右列 | >0 上蓝输出，<0 下绿回收，量程 −100…+300 kW |
| `battery_level` | int | **SOC 数值 + 竖向电量条** | 左列 | ≤15% 变红；充电时变绿 |
| `battery_range_km` | float | 续航 `312 km` | 左列 | 一位取整 |
| `gear` | char | 档位 `P R N D` | 顶部左 | 当前档白色加粗，其余 #333A47 |
| `doors[6]` | u8×6 | **车身俯视图 6 开口** | 底部左 | 打开口填充 #FF453A 并 1Hz 闪烁 |
| `locked` | u8 | 锁图标（开/闭路径） | 顶部右 | 解锁时锁梁抬起 |
| `charging` | u8 | **中央切换为充电面板** | 中区 | ⚡ + 充电功率 + 进度条 + 剩余时间 |
| `charge_power_kw` | float | 充电面板 `110 kW` | 中区 | 绿色，进度条宽 = SOC% |
| `inside_temp` / `outside_temp` | float | `OUT 23.5° / IN 25.0°` | 顶部中 | 1 位小数 |
| `tpms[4]` | float×4 | **四轮胎压 4 格** | 底部右 | <2.2 或 >2.8 bar 红色加粗 |

---

## 3. 布局分区（320×240 像素几何）

```
┌────────────────────────────────────────────────────────────┐
│ P R N D    OUT 23.5° │ IN 25.0°        🔒  ▂▄▆ -61 dBm   │ ← y0–24 状态条
├──────────┬──────────────────────────┬──────────────────────┤
│  78%     │                          │           +45 kW     │
│ ┃████┃   │          128             │    ┃       ┃ 300     │ y24–184
│ ┃████┃   │         km/h             │    ┃       ┃ 0       │
│ 312 km   │                          │    ┃       ┃ -100    │
│ RANGE    │  ⚡充电时切换为充电面板     │    └───────┘ MOTOR   │
├──────────┴──────────────────────────┴──────────────────────┤
│ ▭▭ 车身俯视(6开口) │ TPMS · BAR                            │ ← y184–240
│   开口红闪          │ FL 2.45  FR 2.42  RL 2.40  RR 2.38   │
└────────────────────────────────────────────────────────────┘
   x0        x84        x116            x236          x320
```

| 区域 | 坐标 (x, y, w, h) | 内容 |
| :--- | :--- | :--- |
| 状态条 `hdr` | 0, 0, 320, 24 | 档位 · 内外温 · 锁 · BLE 信号（底边 1px 分割线） |
| 告警条 `banner` | 0, 24, 320, 20 | 覆盖式，半透明红/琥珀，1Hz 闪烁 |
| 左列 `col-left` | 0, 24, 84, 160 | SOC(34px) → 电量条(14×60) → 续航(16px) → `RANGE` 标签(8px, 贴底) |
| 中区 `hero` | 84, 24, 152, 160 | 车速(74px) + `km/h`；充电时整块切换 |
| 右列 `col-right` | 236, 24, 84, 160 | 功率值(21px) → 功率计(14×60) → 量程刻度 → `MOTOR` 标签(贴底) |
| 底部 `ftr` | 0, 184, 320, 56 | 车身俯视 90×42（x7）｜1px 竖分割线(x107)｜TPMS 4 格(x116 起) |
| 断连遮罩 | 0, 24, 320, 216 | 黑 82% 遮罩 + 三点脉冲 + `Searching for Tesla BLE broadcast…` |

**对齐规则**：左右两列数值基线同为 y34–68，电量条与功率计同为 y76–136，两个区域标签同为 y167–178。

---

## 4. 视觉规范

### 4.1 色板（`ui_theme.h` 中定义为 token）

| Token | 值 | 用途 |
| :--- | :--- | :--- |
| `C_BG` | `#000000` | 背景（纯黑省电、车舱融合） |
| `C_FG` | `#FFFFFF` | 车速、SOC、胎压等关键值 |
| `C_DIM` | `#8A93A6` | 单位、辅助值 |
| `C_FAINT` | `#5A6377` | 区域标签 RANGE / TPMS / 量程刻度 |
| `C_LINE` | `#14181F` | 1px 分区线 |
| `C_TRACK` | `#0E1219` / 边框 `#1E2430` | 槽体填充 |
| `C_POWER` | `#3E6AE1` | 驱动功率 > 0 |
| `C_REGEN` | `#21C15E` | 动能回收 < 0、充电中、电量条充电态 |
| `C_ALERT` | `#FF453A` | 开口未关、胎压异常、低电 |
| `C_CAUTION` | `#FFB340` | 二级警告（低电量条） |
| `C_GEAR_OFF` | `#333A47` | 未选中的档位字母 |

### 4.2 字号阶梯

| px | 字重 | 用途 |
| :--- | :--- | :--- |
| 74 | Light(250) | 车速数字（字距 −4，tabular 数字防跳动） |
| 34 | SemiBold | SOC |
| 34(高) | — | 充电功率 38px SemiBold 绿 |
| 21 | SemiBold | 电机功率 |
| 16–17 | Medium | 续航、胎压数值 |
| 13 | Bold | 档位字母 |
| 11–12 | Regular | 温度、单位、充电说明 |
| 10.5 | Bold | 告警条文案 |
| 8 | SemiBold | 区域标签（字距 1.4） |
| 7 | SemiBold | 量程刻度 300 / 0 / −100 |

### 4.3 形状

- 圆角：电量条/功率计 **7px 胶囊**；车身开口区 2.5–8px；告警条直角。
- 线宽：分割线、描边 1px；车身轮廓 1.2px。
- 无阴影、无渐变背景；仅功率条内部用同色系纵向渐变提亮。

---

## 5. 状态机（由 SimPayload 驱动，无需人工切页）

| 状态 | 触发条件 | 界面变化 |
| :--- | :--- | :--- |
| **行驶 `drive`** | 默认，`speed>0, charging=0` | 三区正常显示 |
| **回收 `regen`** | `motor_power_kw < 0` | 功率值/条变绿，条向下生长 |
| **充电 `charging`** | `charging=1` | 中区换充电面板（⚡ + kW + 进度 + ETA），电量条变绿，档位显示 P |
| **车门告警 `doors`** | 任一 `doors[i]=1` | 对应开口红闪 + 顶部红条 `FRUNK + FRONT LEFT DOOR OPEN` |
| **胎压告警 `tpms`** | 任一胎压 <2.2 或 >2.8 | 对应格红字 + 红条 `RL TIRE LOW · 1.90 bar` |
| **低电量 `low`** | `battery_level ≤ 15` | SOC/电量条变红 + 琥珀条 `LOW BATTERY · 48 km RANGE` |
| **断连 `offline`** | `SYNC_TIMEOUT` / BLE 断开 | 全屏遮罩 `--` + 三点脉冲 + 重连文案 |

**告警条规则**：同一时刻只显示 1 条，优先级 `车门 > 胎压 > 低电`；1Hz 闪烁；数据恢复后延迟 3s 消失；断连时强制清空。

---

## 6. 动画与性能预算

| 动画 | 时长/曲线 |
| :--- | :--- |
| 数值补间（车速、SOC、胎压） | 200ms ease-out，800ms 数据到达后执行 |
| 功率条 / 电量条高度 | 300ms `cubic-out` |
| 充电进度条 | 400ms ease |
| 告警闪烁 | 1Hz（steps 方波，非渐隐） |
| 断连三点 | 1.2s 循环脉冲，错相 0.2s |
| 充电 ⚡ 光晕 | 1.6s 呼吸 |

**性能**：
- 局部刷新缓冲 `320×24×2 = 15360B` ×2（双缓冲），占 30KB RAM。
- SPI 80MHz 下全屏约 20–30ms；数据 800ms 一帧绰绰有余，动画期间保持 ≥30fps。
- 只对脏区入队（数值变化才 invalidate），静止画面零开销。

---

## 7. LVGL 对象结构

```
scr_main
├─ hdr        (lv_obj, h24, 无边框, 底边 style 边框线)
│  ├─ gear    4×lv_label（P R N D）
│  ├─ temps   lv_label "OUT 23.5° │ IN 25.0°"
│  └─ sys     lv_image(锁) + lv_image(BLE) + lv_label(dBm)
├─ banner     (lv_obj, h20, 背景半透明红, 可见性切换)
├─ col_left   (lv_obj, w84)
│  ├─ soc     lv_label(34px)
│  ├─ bar     lv_bar 竖置（或自绘 lv_canvas 胶囊）
│  ├─ range   lv_label(16px)
│  └─ cap     lv_label("RANGE")
├─ hero       (lv_obj, w152)
│  ├─ spd     lv_label(74px)  + unit lv_label
│  └─ charge  (lv_obj, 隐藏) bolt lv_image + kW + lv_bar + eta lv_label
├─ col_right  (lv_obj, w84)
│  ├─ pw      lv_label(21px)
│  ├─ gauge   自绘 widget：零线 + 上蓝条 + 下绿条（lv_obj_draw 或 lv_canvas）
│  ├─ ticks   3×lv_label(7px)
│  └─ cap     lv_label("MOTOR")
├─ ftr        (lv_obj, h56)
│  ├─ car     lv_image(底图) + 6×lv_obj 半透明红色矩形作开口高亮
│  ├─ vr      1px lv_obj 分割线
│  └─ tpms    4×(lv_label 分类 + lv_label 数值)
└─ overlay    (lv_obj, 隐藏) 断连文案 + 3 dots
```

**要点**
1. **车身图**：底图用 90×42 RGB565 的 C 数组（≈7.6KB）；6 个开口用可定位的 `lv_obj` 叠加，避免做 2⁶ 张状态图。
2. **字体**：`lv_font_conv --font Montserrat-Light.ttf` 只导出实际用到的字符——74px 仅 `0-9`，34px 加 `%`，其余各档 88 个 ASCII；总字库 < 30KB。
3. **接口**：`ble_adapter.cpp` 收到 53 字节 → 填充 `DashboardData` 结构 → `ui_set_payload(&data)`；UI 层与 BLE 完全解耦。
4. **功率计**为自绘 widget（LVGL 无“零点居中双向条”原生控件），对外暴露 `pw_gauge_set_value(kw)`。

---

## 8. 建议的固件目录

```
firmware/
├─ src/
│  ├─ ble/ble_adapter.cpp        # 53B SimPayload 收包 → DashboardData
│  ├─ ui/ui_theme.h              # 第 4 节色板 token
│  ├─ ui/ui.h / ui.c             # DashboardData、状态机、ui_set_payload()
│  ├─ ui/screens/screen_main.c   # 第 7 节对象树
│  ├─ ui/widgets/pw_gauge.c      # 零点功率计
│  ├─ ui/widgets/car_status.c    # 车身 6 开口
│  ├─ ui/fonts/                  # lv_font_conv 产物
│  └─ ui/assets/car_top.c        # 车身底图
└─ platformio.ini / CMakeLists.txt
```

---

## 9. 下一步

1. 定稿本布局 → 确认后导出各状态切图（可从 `ui-mockup.html` 直接截取）。
2. 搭 LVGL 工程骨架 + `ui_theme.h` + 字体裁剪。
3. 实现 `screen_main.c` 静态版（无动画）→ 接 `DashboardData`。
4. 接仿真器联调：先 `--nogui --vin …` 理想 20Hz，再切 REALISTIC 800ms 验证补间。
5. 可选：2.8" 带触摸（XPT2046）时，加点击翻页（胎压详情 / 里程）与亮度调节。
