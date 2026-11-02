# 特斯拉 ESP32 2.8" 仪表盘 · 界面设计规范 v1.0

> 可视化原型：`dashboard/ui-mockup.html`（浏览器打开，可切换 7 种状态、2:1/1:1 缩放）
> 固件实机对齐预览图：`dashboard/preview/dashboard_states.png`（4 状态）、`dashboard/preview/dashboard_normal_2x.png`，
> 由 `python dashboard/preview/mock_render.py` 生成（Pillow，坐标与本规范同步）
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
| `speed_kmh` | float | **车速大字 48px** | 中区（紧邻车身栏） | 200ms 插值补间；断连显示 `--` |
| `motor_power_kw` | float | **不展示**（v1.0 简约版移除功率计） | — | 数据仍接收，UI 不渲染 |
| `battery_level` | int | **SOC 数值 + 竖向电量条** | 最右列（充电模式隐藏） | ≤15% 变红；充电时变绿 |
| `battery_range_km` | float | 续航 `312 km` | 最右列 | 一位取整 |
| `gear` | char | 档位 `P R N D` | 顶部左 | 当前档白色加粗，其余 #333A47 |
| `doors[6]` | u8×6 | **车身俯视图 6 开口** | 左侧车身栏 | 打开口填充 #FF453A 并 1Hz 闪烁 |
| `locked` | u8 | 文本 `LOCK` / `UNLK` | 顶部右 | 解锁时琥珀色 |
| `charging` | u8 | **右侧整块切换为充电面板**（隐藏电量列，x104–320 全宽） | 中区 | ⚡ + 充电功率(大字+绿色功率柱) + 进度条 + 剩余时间 |
| `charge_power_kw` | float | 充电面板 `110 kW` | 中区（全宽） | 绿色大字 + 绿色功率柱（0–250 kW 满量程）；SOC 进度条宽 = SOC% |
| `inside_temp` / `outside_temp` | float | `OUT 23.5° / IN 25.0°` | 顶部中 | 1 位小数 |
| `tpms[4]` | float×4 | **四轮胎压：四轮着色 + 各轮外侧「轮位/数值/迷你量程条」** | 左侧车身栏，与车轮一一对应 | 车轮/量程条中性灰=正常、红+1Hz 闪烁=`<2.8` 或 `>3.1` bar、暗灰=断连；数值正常白色，异常红色 |

---

## 3. 布局分区（320×240 像素几何）

```
┌──────────────────────┬─────────────────────┬───────────────┐
│ P R N D  OUT 23.5°│IN 25.0°      LOCK  LINK │ ← y0–24 状态条
├──────────────────────┼─────────────────────┼───────────────┤
│ TIRE PRESSURE        │                     │  78%          │
│  FL 2.9▬    FR 3.0▬  │       128           │  ▮            │
│   ▯ 车身俯视  ▯      │                     │  ▮  电量条     │
│  RL 2.8▬    RR 3.1▬  │      KM/H           │  ▮  14×110    │
│                      │                     │               │
│                      │                     │  312 km       │
│                      │                     │  RANGE        │
├──────────────────────┴─────────────────────┴───────────────┤
│ ⚠ RL TIRE 1.90 bar                     (y224 告警 ticker)  │
└────────────────────────────────────────────────────────────┘
  x0-104 车身栏   x104-250 车速区   x250-320 电量列
  车头朝上；胎压块 FL/RL 在车左外侧，FR/RR 在车右外侧，与车轮同高
  充电时：中区扩为 x104-320 全宽显示充电面板，电量列隐藏
```

| 区域 | 坐标 (x, y, w, h) | 内容 |
| :--- | :--- | :--- |
| 状态条 `hdr` | 0, 0, 320, 24 | 档位 · `OUT/IN` 小标签+温度 · 锁 · BLE 状态（底边 1px 分割线） |
| 告警条 `banner` | 0, 224, 320, 16 | **屏幕最底部** ticker，半透明红/琥珀，1Hz 闪烁（不再遮挡 SOC/车速） |
| 车身栏 `carcol` | 0, 24, 104, 216 | `TIRE PRESSURE` 8px 标签 → 车身俯视 52×120（前后风挡 + 6 开口 + 四轮着色）→ 每轮「轮位 8px + 数值 10px + 迷你量程条 16×3」贴在轮外侧 |
| 中区 `hero` | 104, 24, 146, 216 | 车速(48px，内置最大号) + `KM/H`（12px，字距 3）；**充电模式扩展为 x104–320 全宽(216px)**，仅显示充电数据 |
| 电量列 `col-batt` | 250, 24, 70, 216 | `SOC 数字(26px) + %(12px)` → 电量条(14×110) → 续航(16px) → `RANGE` 标签(8px)；左侧 1px 分割线；**充电模式隐藏** |
| 断连/连接遮罩 | 0, 24, 320, 216 | 全黑遮罩状态页：`TESLA BLE LINK` 小标签(8px,字距2) → 状态大字(26px: SCANNING/CONNECTING/SYNCING/RECONNECTING) → 说明(12px) → 三点脉冲 → `SCAN › CONNECT › SYNC` 阶段面包屑(完成绿/当前琥珀/未到暗灰) → VIN(8px) → `ELAPSED n s` → 底部提示(8px) |

**对齐规则**：SOC 与温度数值顶部对齐（y32–62）；四轮胎压块与车轮同高（y98/142 起）；
电量条 y70–180，与中区车速块（y94–170）垂直居中；区域标签统一 8px + 字距 1。

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
| `C_POWER` | `#3E6AE1` | 预留（v1.0 简约版不展示电机功率） |
| `C_REGEN` | `#21C15E` | 动能回收 < 0、充电中、电量条充电态 |
| `C_ALERT` | `#FF453A` | 开口未关、胎压异常、低电 |
| `C_CAUTION` | `#FFB340` | 二级警告（低电量条） |
| `C_GEAR_OFF` | `#333A47` | 未选中的档位字母 |

### 4.2 字号阶梯

| px | 字重 | 用途 |
| :--- | :--- | :--- |
| 48 | Light(250) | 车速数字（内置最大号，tabular 数字理想；当前用 montserrat_48） |
| 26 | SemiBold | SOC / 充电功率 kW |
| 16 | Medium | 续航、告警条 |
| 14 | Bold | 档位字母、内外温、锁状态 |
| 12 | Regular | 温度单位、胎压单位、`KM/H` |
| 10 | Regular | 胎压数值、BLE 状态 |
| 8 | SemiBold | 区域标签（字距 1.0–1.4） |

> 字体只启用 LVGL 内置 Montserrat 8/10/12/14/16/26/48；CJK 需自行裁剪，屏幕文案保持 ASCII。

### 4.3 形状

- 圆角：电量条 **7px 胶囊（14×110，纯色填充）**；功率计 5px 胶囊（10×110）；车身开口区 2px；告警条直角。
- 线宽：分割线、描边 1px；车身轮廓 1px（`C_CAR_LINE 0x39414F`，比分割线亮一档）。
- 无阴影、无渐变；所有进度条均为手绘 `lv_obj` 方块（`LV_USE_BAR=0`），避免额外控件开销。

---

## 5. 状态机（由 SimPayload 驱动，无需人工切页）

| 状态 | 触发条件 | 界面变化 |
| :--- | :--- | :--- |
| **行驶 `drive`** | 默认，`speed>0, charging=0` | 三区正常显示 |
| **回收 `regen`** | `motor_power_kw < 0` | 功率值/条变绿，条向下生长 |
| **充电 `charging`** | `charging=1` | 中区换充电面板（⚡ + kW + 进度 + ETA），电量条变绿，档位显示 P |
| **车门告警 `doors`** | 任一 `doors[i]=1` | 对应开口红闪 + 底部红条 `FL DOOR OPEN` / `FRUNK OPEN` … |
| **胎压告警 `tpms`** | 任一胎压 `<2.8` 或 `>3.1` | 对应轮位/数值/迷你条变红 + 底部红条 `RL TIRE 1.90 bar` |
| **低电量 `low`** | `battery_level ≤ 15` | SOC 与电量条变红 + 琥珀条 `LOW BATTERY · 48 km RANGE` |
| **断连/未就绪 `offline`** | BLE 未 Ready，或 Ready 但数据/握手未就绪 | 全黑状态遮罩：状态大字 + 说明 + 三点脉冲 + `SCAN › CONNECT › SYNC` 进程 + VIN + 已耗时 + 提示（按 BLE 状态切换标题：SCANNING/CONNECTING/SYNCING/RECONNECTING） |

**告警条规则**：位于屏幕最底部 `y224, h16` 的 ticker（不覆盖 SOC/车速/车身）；同一时刻只显示 1 条，优先级 `车门 > 胎压 > 低电`；文案前自动加 `LV_SYMBOL_WARNING`；1Hz 闪烁（背景不透明度 60% ↔ 20%）；断连时强制隐藏。

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

**内存**（实测约束，勿随意改）：
- LVGL 对象池 = **64KB，整块从 ESP32 堆申请**（`lv_conf.h` 的 `LV_MEM_POOL_ALLOC → lvglPoolAlloc()`，16 字节对齐）。
  不能放静态数组：`dram0_0_seg` 只剩几 KB（24KB 静态池就 `overflowed`）。
- TLSF 的**单池上限由 `LV_MEM_SIZE` 决定**（`block_size_max = 1 << log2ceil(LV_MEM_SIZE)`）：
  `LV_MEM_SIZE` 必须 ≥ 实际池大小，且用 `lv_mem_add_pool()` 追加比它大的池会被静默拒绝（返回 0，`LV_USE_LOG=0` 时无任何输出）。
- 池不足的典型症状：`StoreProhibited` / `LoadProhibited`、`EXCVADDR 0x8`，backtrace 落在 `lv_draw_add_task` 或 `lv_obj_add_style`（malloc/realloc 返回 NULL 后 LVGL 不检查）。
- `lv_conf.h` 打开了 `LV_USE_ASSERT_MALLOC/NULL`，断言处理器 `lvglAssertFailed()` 打串口后重启：
  再出现内存或空指针问题会看到 `错误: LVGL 断言失败 (文件:行)`，而不是 Guru Meditation。
- 启动日志会打印池总量 + 对象树建好后的用量；`Display::loop()` 每 30s 采样 `lv_mem_monitor`，`used_pct ≥ 85%` 立即告警。
- 完整排查过程见 `docs/LVGL_MEMORY.md`（现象 → addr2line 定位 → 四层根因 → 最终配置）。

---

## 7. LVGL 对象结构

```
scr_main
├─ hdr        (lv_obj, h24, 底边 1px style 边框线)
│  ├─ gear    4×lv_label(14px, P R N D)
│  ├─ outCap  lv_label(8px "OUT", 字距 1)
│  ├─ outVal  lv_label(14px, "23.5°")
│  ├─ tempDiv lv_obj(1×10 竖线)
│  ├─ inCap   lv_label(8px "IN")
│  ├─ inVal   lv_label(14px)
│  ├─ lockLbl lv_label(14px, LOCK/UNLK)
│  └─ bleLbl  lv_label(10px, LINK/SCAN/KEY…)
├─ carcol     (lv_obj, 0,24, 104×216)
│  ├─ cap     lv_label(8px "TIRE PRESSURE", 字距 1)
│  ├─ body    lv_obj(52×120 胶囊, 描边 C_CAR_LINE) 
│  │  ├─ spine/glassF/glassR  中线 + 前后风挡色带
│  │  ├─ doorObj[6]  lv_obj(2px 圆角, 关闭 #14181F / 打开红闪)
│  │  └─ wheelObj[4] lv_obj(6×24, 正常灰 / 异常红 / 断连暗)
│  └─ tpms×4  每组：tpmsName(8px) + tpmsVal(10px) + tpmsBarBg/tpmsBar(16×3)
├─ hero       (lv_obj, 104,24, 行驶 w146 / 充电 w216)
│  ├─ spdLbl  lv_label(48px)
│  ├─ spdUnit lv_label(12px "KM/H", 字距 3)
│  └─ chargePanel (lv_obj, 隐藏) bolt 图 + kW(26px) + 功率柱(10×110) + SOC 横条(188×10) + ETA
├─ colBatt    (lv_obj, 250,24, 70×216, 左边框 1px; 充电模式隐藏)
│  ├─ socLbl  lv_label(26px, 右对齐)
│  ├─ socPct  lv_label(12px "%")
│  ├─ battTrack/battFill  lv_obj(14×110 胶囊, 从底部长)
│  ├─ rangeLbl lv_label(16px "312 km")
│  └─ battCap lv_label(8px "RANGE", 字距 1)
├─ banner     (lv_obj, 0,224, 320×16, 半透明红, 可见性 + 1Hz 闪烁)
├─ overlay    (lv_obj, 0,24, 320×216, 隐藏) 状态页：cap + title(26px) + sub(12px)
│   + dots[3] + step[3](SCAN/CONNECT/SYNC) + target(VIN) + elapsed + hint
└─ pairCard   (lv_obj, 0,24, 320×216, 配钥引导卡片, 覆盖 overlay)
```

**要点**
1. **车身图不用位图**：52×120 圆角矩形 + 中线 + 前后风挡色带矢量绘制，省 ~12KB Flash；6 个开口和 4 个车轮都是可定位的 `lv_obj`，关闭的开口画成暗色区块而非隐藏，车身结构始终可读。
2. **字体**：只启用 LVGL 内置 `Montserrat 8/10/12/14/16/26/48`（`lv_conf.h`），无 56px 档；屏幕文案全 ASCII。
3. **接口**：`main.cpp` 组好 `Dashboard::Vehicle` + `PairingHint` 后调用 `dashboard.render(...)`；UI 层与 BLE 完全解耦。
4. **进度条**：`LV_USE_BAR/ARC=0`，电量条、功率柱、SOC 横条、迷你量程条全部用手绘 `lv_obj` 改宽高实现，只在数值变化时 `invalidate`。

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
