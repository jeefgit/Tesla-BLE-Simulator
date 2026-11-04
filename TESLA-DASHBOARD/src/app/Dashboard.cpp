#include "app/Dashboard.h"

#include <stdio.h>
#include <string.h>

#include "config.h"

// ---------------------------------------------------------------------------
// 脏写守卫：LVGL 的 set_text/set_style_* 即使值相同也会标脏重绘，
// 高频无条件写会让整屏反复重绘（表现为屏幕闪动感）。全部改为变化才写。
// ---------------------------------------------------------------------------
static void setText(lv_obj_t* lbl, const char* s) {
  const char* cur = lv_label_get_text(lbl);
  if (cur == nullptr || strcmp(cur, s) != 0) lv_label_set_text(lbl, s);
}

static void setTextColorC(lv_obj_t* o, uint32_t c) {
  // 目标色与当前色都先量化到 RGB565 再比较（round-trip 不等于原值）
  const uint32_t q = lv_color_to_u32(lv_color_hex(c));
  if (lv_color_to_u32(lv_obj_get_style_text_color(o, LV_PART_MAIN)) == q) {
    return;
  }
  lv_obj_set_style_text_color(o, lv_color_hex(c), LV_PART_MAIN);
}

static void setBgColorC(lv_obj_t* o, uint32_t c) {
  const uint32_t q = lv_color_to_u32(lv_color_hex(c));
  if (lv_color_to_u32(lv_obj_get_style_bg_color(o, LV_PART_MAIN)) == q) {
    return;
  }
  lv_obj_set_style_bg_color(o, lv_color_hex(c), LV_PART_MAIN);
}

static void setHeightIfChanged(lv_obj_t* o, int32_t h) {
  if (lv_obj_get_height(o) != h) lv_obj_set_height(o, h);
}

static void setPosIfChanged(lv_obj_t* o, int32_t x, int32_t y) {
  if (lv_obj_get_x(o) != x || lv_obj_get_y(o) != y) lv_obj_set_pos(o, x, y);
}

// ---------------------------------------------------------------------------
// 色板 token（dashboard/DESIGN.md §4.1，RGB565 经 lv_color_hex 转换）
// ---------------------------------------------------------------------------
namespace {
constexpr uint32_t C_BG = 0x000000;
constexpr uint32_t C_FG = 0xFFFFFF;
constexpr uint32_t C_DIM = 0x8A93A6;
constexpr uint32_t C_FAINT = 0x5A6377;
constexpr uint32_t C_LINE = 0x14181F;
constexpr uint32_t C_TRACK = 0x0E1219;
constexpr uint32_t C_TRACK_BRD = 0x1E2430;
constexpr uint32_t C_REGEN = 0x21C15E;
constexpr uint32_t C_ALERT = 0xFF453A;
constexpr uint32_t C_CAUTION = 0xFFB340;
constexpr uint32_t C_GEAR_OFF = 0x333A47;
constexpr uint32_t C_TPMS_OK = 0x4A5163;   // 胎压正常中性灰
constexpr uint32_t C_OFFLINE = 0x232A36;   // 断连暗灰

// 胎压正常区间（bar），越界变红闪烁（DESIGN.md §5）
constexpr float TPMS_LO = 2.8f;
constexpr float TPMS_HI = 3.1f;

// 车身轮廓色（比分区线亮一档，黑色背景上才看得见）
constexpr uint32_t C_CAR_LINE = 0x39414F;
constexpr uint32_t C_DOOR_OFF = 0x14181F;

const char* bleStateText(BleLink::State s) {
  switch (s) {
    case BleLink::State::Idle: return "--";
    case BleLink::State::Scanning: return "SCAN";
    case BleLink::State::Connecting: return "CONN";
    case BleLink::State::Ready: return "LINK";
    case BleLink::State::Reconnecting: return "RETRY";
  }
  return "--";
}

// 去掉 LVGL 默认内边距/滚动条，所有布局用手写坐标（DESIGN.md §3）
void strip(lv_obj_t* o) {
  lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_row(o, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_column(o, 0, LV_PART_MAIN);
  lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_scroll_dir(o, LV_DIR_NONE);
}

lv_obj_t* mkObj(lv_obj_t* parent, int32_t x, int32_t y, int32_t w,
                int32_t h) {
  lv_obj_t* o = lv_obj_create(parent);
  strip(o);
  lv_obj_remove_style_all(o);  // 完全裸容器，样式全手写
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  return o;
}

lv_obj_t* mkLabel(lv_obj_t* parent, int32_t x, int32_t y, int32_t w,
                  int32_t h, const lv_font_t* font, uint32_t color,
                  lv_text_align_t align = LV_TEXT_ALIGN_CENTER) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_pos(l, x, y);
  lv_obj_set_size(l, w, h);
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
  lv_obj_set_style_text_color(l, lv_color_hex(color), LV_PART_MAIN);
  lv_obj_set_style_text_align(l, align, LV_PART_MAIN);
  lv_obj_set_style_pad_all(l, 0, LV_PART_MAIN);
  lv_obj_set_scrollbar_mode(l, LV_SCROLLBAR_MODE_OFF);
  return l;
}

// 车身俯视图几何（carcol 内相对坐标，车头朝上，车身 52×120）
// DESIGN.md §7：底图用圆角矩形组合代替 C 数组位图（省 12KB Flash，矢量更清晰）
constexpr int32_t CAR_X = 26;   // carcol 宽 104，车身居中
constexpr int32_t CAR_Y = 48;
constexpr int32_t CAR_W = 52;
constexpr int32_t CAR_H = 120;

// 6 开口高亮位置（相对 carcol）：顶前盖 / 左前 / 右前 / 左后 / 右后 / 尾箱
// 与 TeslaPayload doors[6] 顺序 fl,fr,rl,rr,frunk,trunk 一一对应
const int32_t kDoorXY[6][4] = {
    {CAR_X + 2, CAR_Y + 22, 5, 30},          // FL 左前门
    {CAR_X + CAR_W - 7, CAR_Y + 22, 5, 30},  // FR 右前门
    {CAR_X + 2, CAR_Y + 60, 5, 30},          // RL 左后门
    {CAR_X + CAR_W - 7, CAR_Y + 60, 5, 30},  // RR 右后门
    {CAR_X + 12, CAR_Y + 2, CAR_W - 24, 6},  // FRUNK 前盖
    {CAR_X + 12, CAR_Y + CAR_H - 8, CAR_W - 24, 6},  // TRUNK 尾箱
};

// 4 车轮位置（相对 carcol）：与 tpms[4] fl,fr,rl,rr 对应
const int32_t kWheelXY[4][4] = {
    {CAR_X - 6, CAR_Y + 26, 6, 24},  // FL
    {CAR_X + CAR_W, CAR_Y + 26, 6, 24},  // FR
    {CAR_X - 6, CAR_Y + 70, 6, 24},  // RL
    {CAR_X + CAR_W, CAR_Y + 70, 6, 24},  // RR
};

// 胎压数据块（轮位 + 数值 + 迷你量程条）：贴在车轮外侧的 20px 空隙里
const int32_t kTpmsBlkXY[4][2] = {
    {0, CAR_Y + 26},                 // FL 左外
    {CAR_W + 32, CAR_Y + 26},        // FR 右外
    {0, CAR_Y + 70},                 // RL 左外
    {CAR_W + 32, CAR_Y + 70},        // RR 右外
};
constexpr int32_t kTpmsBlkW = 20;
constexpr int32_t kTpmsBarW = 16;  // 迷你量程条满宽
}  // namespace

// ---------------------------------------------------------------------------
// 构建静态对象树（只建一次，之后 updateAll 只改属性）
// ---------------------------------------------------------------------------
void Dashboard::begin() {
  buildUi();
  lv_timer_create(&Dashboard::timerCb, 50, this);  // 50ms 动画节拍

  // 对象树占了多少 LVGL 堆：内存不够时 LVGL 会静默拿到 NULL 指针然后崩，
  // 所以这里先打一行，崩溃在首帧之前也能看到真实用量。
  lv_mem_monitor_t mon;
  lv_mem_monitor(&mon);
  CFG_LOG("仪表层就绪（LVGL 对象树, %dx%d）: 堆 %u%% 已用 (%u/%u B), 峰值 %u B",
          CFG_LCD_WIDTH, CFG_LCD_HEIGHT, (unsigned)mon.used_pct,
          (unsigned)(mon.total_size - mon.free_size),
          (unsigned)mon.total_size, (unsigned)mon.max_used);
}

void Dashboard::buildUi() {
  lv_obj_t* scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), LV_PART_MAIN);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  buildHeader();     // y0-24
  buildCarColumn();  // x0-104, y24-240
  buildHero();       // x104-250, y24-240（充电时扩展到 x320）
  buildBatteryColumn();  // x250-320, y24-240（充电时隐藏）
  buildBanner();     // y224-240 底部告警 ticker（最后建保证 z 序在最上层）
  buildOverlay();    // 断连遮罩
  buildPairCard();   // 配钥引导卡片（最顶层）
}

void Dashboard::buildHeader() {
  hdr = mkObj(lv_screen_active(), 0, 0, CFG_LCD_WIDTH, 24);
  // 底边 1px 分割线
  lv_obj_set_style_border_side(hdr, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
  lv_obj_set_style_border_width(hdr, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(hdr, lv_color_hex(C_LINE), LV_PART_MAIN);
  lv_obj_set_style_bg_color(hdr, lv_color_hex(C_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, LV_PART_MAIN);

  // 档位 P R N D（左对齐一排，当前档白色、其余暗灰）
  const char gears[4] = {'P', 'R', 'N', 'D'};
  for (int i = 0; i < 4; ++i) {
    gearLbl[i] = mkLabel(hdr, 9 + i * 18, 3, 16, 18,
                         &lv_font_montserrat_14, C_GEAR_OFF);
    lv_obj_set_style_text_letter_space(gearLbl[i], 1, LV_PART_MAIN);
    lv_label_set_text_fmt(gearLbl[i], "%c", gears[i]);
  }

  // 内外温：小号 OUT/IN 标签 + 大号数值，中间 1px 竖线分隔
  outCap = mkLabel(hdr, 88, 9, 20, 10, &lv_font_montserrat_8, C_FAINT,
                   LV_TEXT_ALIGN_LEFT);
  lv_obj_set_style_text_letter_space(outCap, 1, LV_PART_MAIN);
  lv_label_set_text(outCap, "OUT");

  outVal = mkLabel(hdr, 108, 4, 36, 16, &lv_font_montserrat_14, C_FG,
                   LV_TEXT_ALIGN_LEFT);
  lv_label_set_text(outVal, "--.-");

  tempDiv = mkObj(hdr, 146, 7, 1, 10);
  lv_obj_set_style_bg_color(tempDiv, lv_color_hex(C_TRACK_BRD), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(tempDiv, LV_OPA_COVER, LV_PART_MAIN);

  inCap = mkLabel(hdr, 152, 9, 14, 10, &lv_font_montserrat_8, C_FAINT,
                  LV_TEXT_ALIGN_LEFT);
  lv_obj_set_style_text_letter_space(inCap, 1, LV_PART_MAIN);
  lv_label_set_text(inCap, "IN");

  inVal = mkLabel(hdr, 168, 4, 36, 16, &lv_font_montserrat_14, C_FG,
                  LV_TEXT_ALIGN_LEFT);
  lv_label_set_text(inVal, "--.-");

  // 锁状态 + BLE 状态（右侧）
  lockLbl = mkLabel(hdr, 240, 4, 34, 16, &lv_font_montserrat_14, C_DIM,
                    LV_TEXT_ALIGN_RIGHT);
  lv_label_set_text(lockLbl, "UNLK");
  bleLbl = mkLabel(hdr, 278, 6, 38, 12, &lv_font_montserrat_10, C_FAINT,
                   LV_TEXT_ALIGN_RIGHT);
  lv_label_set_text(bleLbl, "--");
}

void Dashboard::buildBanner() {
  // 告警条放屏幕最底部：顶部状态栏和 SOC/车速都不被遮住，
  // 16px 高、整宽，1Hz 闪烁（DESIGN.md 原设计是覆盖式，这里改为底部 ticker）
  banner = mkObj(lv_screen_active(), 0, CFG_LCD_HEIGHT - 16, CFG_LCD_WIDTH, 16);
  lv_obj_set_style_bg_color(banner, lv_color_hex(C_ALERT), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(banner, LV_OPA_60, LV_PART_MAIN);
  lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN);

  bannerLbl = mkLabel(banner, 0, 0, CFG_LCD_WIDTH, 16,
                      &lv_font_montserrat_14, C_FG);
  lv_label_set_text(bannerLbl, "");
}

void Dashboard::buildCarColumn() {
  carcol = mkObj(lv_screen_active(), 0, 24, 104, 216);
  // 右边框 1px 分割
  lv_obj_set_style_border_side(carcol, LV_BORDER_SIDE_RIGHT, LV_PART_MAIN);
  lv_obj_set_style_border_width(carcol, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(carcol, lv_color_hex(C_LINE),
                                LV_PART_MAIN);

  // 区域标签：8px 大写 + 字距（DESIGN.md §4.2）
  lv_obj_t* cap = mkLabel(carcol, 0, 6, 104, 10, &lv_font_montserrat_8,
                          C_FAINT);
  lv_obj_set_style_text_letter_space(cap, 1, LV_PART_MAIN);
  lv_label_set_text(cap, "TIRE PRESSURE");

  // 车身轮廓（圆角矩形，车头朝上）
  lv_obj_t* body = mkObj(carcol, CAR_X, CAR_Y, CAR_W, CAR_H);
  lv_obj_set_style_radius(body, 14, LV_PART_MAIN);
  lv_obj_set_style_bg_color(body, lv_color_hex(C_TRACK), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(body, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(body, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(body, lv_color_hex(C_CAR_LINE),
                                LV_PART_MAIN);
  // 车顶中线（1px 竖线，示意轴线）
  lv_obj_t* spine = mkObj(body, CAR_W / 2, 8, 1, CAR_H - 16);
  lv_obj_set_style_bg_color(spine, lv_color_hex(C_LINE), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(spine, LV_OPA_COVER, LV_PART_MAIN);

  // 前后风挡：两块深色圆角带，让俯视图能看出车头/车尾
  lv_obj_t* glassF = mkObj(body, 5, 16, CAR_W - 10, 10);
  lv_obj_set_style_radius(glassF, 4, LV_PART_MAIN);
  lv_obj_set_style_bg_color(glassF, lv_color_hex(0x171C26), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(glassF, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_t* glassR = mkObj(body, 5, CAR_H - 26, CAR_W - 10, 10);
  lv_obj_set_style_radius(glassR, 4, LV_PART_MAIN);
  lv_obj_set_style_bg_color(glassR, lv_color_hex(0x171C26), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(glassR, LV_OPA_COVER, LV_PART_MAIN);

  // 6 开口（默认暗色区块 = 关闭，打开时红色 + 1Hz 闪烁）
  for (int i = 0; i < 6; ++i) {
    doorObj[i] = mkObj(carcol, kDoorXY[i][0], kDoorXY[i][1],
                       kDoorXY[i][2], kDoorXY[i][3]);
    lv_obj_set_style_radius(doorObj[i], 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(doorObj[i], lv_color_hex(C_DOOR_OFF),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(doorObj[i], LV_OPA_COVER, LV_PART_MAIN);
  }

  // 4 车轮（着色随胎压状态变化）
  for (int i = 0; i < 4; ++i) {
    wheelObj[i] = mkObj(carcol, kWheelXY[i][0], kWheelXY[i][1],
                        kWheelXY[i][2], kWheelXY[i][3]);
    lv_obj_set_style_radius(wheelObj[i], 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(wheelObj[i], lv_color_hex(C_TPMS_OK),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(wheelObj[i], LV_OPA_COVER, LV_PART_MAIN);
  }

  // 4×胎压块：轮位(8px) + 数值(12px) + 迷你量程条(20×3)，贴在车轮外侧
  static const char* const kWheelName[4] = {"FL", "FR", "RL", "RR"};
  for (int i = 0; i < 4; ++i) {
    const int32_t bx = kTpmsBlkXY[i][0];
    const int32_t by = kTpmsBlkXY[i][1];

    tpmsName[i] = mkLabel(carcol, bx, by, kTpmsBlkW, 8,
                          &lv_font_montserrat_8, C_FAINT);
    lv_obj_set_style_text_letter_space(tpmsName[i], 1, LV_PART_MAIN);
    lv_label_set_text(tpmsName[i], kWheelName[i]);

    tpmsVal[i] = mkLabel(carcol, bx, by + 9, kTpmsBlkW, 12,
                         &lv_font_montserrat_10, C_FG);
    lv_label_set_text(tpmsVal[i], "--");

    tpmsBarBg[i] = mkObj(carcol, bx + 2, by + 23, kTpmsBarW, 3);
    lv_obj_set_style_radius(tpmsBarBg[i], 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(tpmsBarBg[i], lv_color_hex(C_TRACK),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tpmsBarBg[i], LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(tpmsBarBg[i], 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(tpmsBarBg[i], lv_color_hex(C_TRACK_BRD),
                                  LV_PART_MAIN);

    tpmsBar[i] = mkObj(carcol, bx + 2, by + 23, 0, 3);
    lv_obj_set_style_radius(tpmsBar[i], 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(tpmsBar[i], lv_color_hex(C_TPMS_OK),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tpmsBar[i], LV_OPA_COVER, LV_PART_MAIN);
  }
}

void Dashboard::buildHero() {
  hero = mkObj(lv_screen_active(), 104, 24, 146, 216);

  // 车速大字（48px，垂直居中于 hero）
  spdLbl = mkLabel(hero, 0, 70, 146, 56, &lv_font_montserrat_48, C_FG);
  lv_label_set_text(spdLbl, "--");

  spdUnit = mkLabel(hero, 0, 130, 146, 16, &lv_font_montserrat_12, C_DIM);
  lv_obj_set_style_text_letter_space(spdUnit, 3, LV_PART_MAIN);
  lv_label_set_text(spdUnit, "KM/H");

  // ---- 充电面板（默认隐藏，charging=1 时替换整个中右区）----
  chargePanel = mkObj(hero, 0, 0, 216, 216);
  lv_obj_add_flag(chargePanel, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t* bolt = mkLabel(chargePanel, 14, 24, 40, 40,
                           &lv_font_montserrat_26, C_REGEN);
  lv_label_set_text(bolt, LV_SYMBOL_CHARGE);

  chargeKwLbl = mkLabel(chargePanel, 60, 24, 146, 48,
                        &lv_font_montserrat_26, C_REGEN,
                        LV_TEXT_ALIGN_LEFT);
  lv_label_set_text(chargeKwLbl, "0.0 kW");

  // 充电功率柱（竖置 0-250kW 满量程，右侧）
  chargeTrack = mkObj(chargePanel, 196, 20, 10, 110);
  lv_obj_set_style_radius(chargeTrack, 5, LV_PART_MAIN);
  lv_obj_set_style_bg_color(chargeTrack, lv_color_hex(C_TRACK),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(chargeTrack, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(chargeTrack, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(chargeTrack, lv_color_hex(C_TRACK_BRD),
                                LV_PART_MAIN);
  chargeFill = mkObj(chargePanel, 196, 20, 10, 0);
  lv_obj_set_style_radius(chargeFill, 5, LV_PART_MAIN);
  lv_obj_set_style_bg_color(chargeFill, lv_color_hex(C_REGEN),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(chargeFill, LV_OPA_COVER, LV_PART_MAIN);

  // SOC 进度条（横向胶囊）
  lv_obj_t* socTrack = mkObj(chargePanel, 14, 150, 188, 10);
  lv_obj_set_style_radius(socTrack, 5, LV_PART_MAIN);
  lv_obj_set_style_bg_color(socTrack, lv_color_hex(C_TRACK),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(socTrack, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(socTrack, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(socTrack, lv_color_hex(C_TRACK_BRD),
                                LV_PART_MAIN);
  chargeSocBar = mkObj(chargePanel, 14, 150, 0, 10);
  lv_obj_set_style_radius(chargeSocBar, 5, LV_PART_MAIN);
  lv_obj_set_style_bg_color(chargeSocBar, lv_color_hex(C_REGEN),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(chargeSocBar, LV_OPA_COVER, LV_PART_MAIN);

  chargeEtaLbl = mkLabel(chargePanel, 14, 172, 188, 16,
                         &lv_font_montserrat_14, C_DIM);
  lv_label_set_text(chargeEtaLbl, "EST --:--");
}

void Dashboard::buildBatteryColumn() {
  colBatt = mkObj(lv_screen_active(), 250, 24, 70, 216);
  // 左侧 1px 分割线，让电量列与中区有结构边界
  lv_obj_set_style_border_side(colBatt, LV_BORDER_SIDE_LEFT, LV_PART_MAIN);
  lv_obj_set_style_border_width(colBatt, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(colBatt, lv_color_hex(C_LINE), LV_PART_MAIN);

  // SOC 数值 + 小号 % 单位
  socLbl = mkLabel(colBatt, 0, 8, 54, 30, &lv_font_montserrat_26, C_FG,
                   LV_TEXT_ALIGN_RIGHT);
  lv_label_set_text(socLbl, "--");
  socPct = mkLabel(colBatt, 55, 22, 15, 16, &lv_font_montserrat_12, C_DIM,
                   LV_TEXT_ALIGN_LEFT);
  lv_label_set_text(socPct, "%");

  // 竖向电量条（14×110 胶囊，y46-156，从底部长）
  battTrack = mkObj(colBatt, 28, 46, 14, 110);
  lv_obj_set_style_radius(battTrack, 7, LV_PART_MAIN);
  lv_obj_set_style_bg_color(battTrack, lv_color_hex(C_TRACK),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(battTrack, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(battTrack, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(battTrack, lv_color_hex(C_TRACK_BRD),
                                LV_PART_MAIN);
  // 填充从底部往上长（自底对齐，高度随 SOC）
  battFill = mkObj(colBatt, 28, 46, 14, 0);
  lv_obj_set_style_radius(battFill, 7, LV_PART_MAIN);
  lv_obj_set_style_bg_color(battFill, lv_color_hex(C_FG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(battFill, LV_OPA_COVER, LV_PART_MAIN);

  // 续航 + 区域标签
  rangeLbl = mkLabel(colBatt, 0, 164, 70, 20, &lv_font_montserrat_16,
                     C_FG);
  lv_label_set_text(rangeLbl, "-- km");

  battCap = mkLabel(colBatt, 0, 190, 70, 10,
                    &lv_font_montserrat_8, C_FAINT);
  lv_obj_set_style_text_letter_space(battCap, 1, LV_PART_MAIN);
  lv_label_set_text(battCap, "RANGE");
}

void Dashboard::buildOverlay() {
  overlay = mkObj(lv_screen_active(), 0, 24, CFG_LCD_WIDTH, 216);
  lv_obj_set_style_bg_color(overlay, lv_color_hex(C_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);

  // 顶部区域小标签（与主数据页同一套 8px 字距 2 的语言）
  ovCap = mkLabel(overlay, 0, 10, CFG_LCD_WIDTH, 10, &lv_font_montserrat_8,
                  C_FAINT);
  lv_obj_set_style_text_letter_space(ovCap, 2, LV_PART_MAIN);
  lv_label_set_text(ovCap, "TESLA BLE LINK");

  // 状态大字 + 说明
  ovTitle = mkLabel(overlay, 0, 28, CFG_LCD_WIDTH, 32,
                    &lv_font_montserrat_26, C_FG);
  lv_label_set_text(ovTitle, "SCANNING");

  ovSub = mkLabel(overlay, 0, 66, CFG_LCD_WIDTH, 16,
                  &lv_font_montserrat_12, C_DIM);
  lv_label_set_text(ovSub, "SEARCHING FOR TESLA BROADCAST");

  // 三点脉冲（错相 200ms，颜色随状态走）
  for (int i = 0; i < 3; ++i) {
    dots[i] = mkObj(overlay, CFG_LCD_WIDTH / 2 - 20 + i * 14, 92, 6, 6);
    lv_obj_set_style_radius(dots[i], 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dots[i], lv_color_hex(C_CAUTION),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dots[i], LV_OPA_40, LV_PART_MAIN);
  }

  // 阶段面包屑：SCAN — CONNECT — SYNC（done 绿 / active 琥珀 / pending 暗灰）
  static const char* const kSteps[3] = {"SCAN", "CONNECT", "SYNC"};
  const int32_t xPos[3] = {76, 128, 200};
  const int32_t wPos[3] = {44, 64, 44};
  for (int i = 0; i < 3; ++i) {
    ovStep[i] = mkLabel(overlay, xPos[i], 116, wPos[i], 10,
                        &lv_font_montserrat_8, C_GEAR_OFF);
    lv_obj_set_style_text_letter_space(ovStep[i], 1, LV_PART_MAIN);
    lv_label_set_text(ovStep[i], kSteps[i]);
  }
  for (int i = 0; i < 2; ++i) {
    lv_obj_t* sep = mkLabel(overlay, i == 0 ? 120 : 192, 116, 8, 10,
                            &lv_font_montserrat_8, C_GEAR_OFF);
    lv_label_set_text(sep, ">");
  }

  // 目标（VIN）与耗时；断连时可以据此确认扫的是哪台车
  ovTarget = mkLabel(overlay, 0, 142, CFG_LCD_WIDTH, 10,
                     &lv_font_montserrat_8, C_FAINT);
  lv_obj_set_style_text_letter_space(ovTarget, 1, LV_PART_MAIN);
  lv_label_set_text(ovTarget, "VIN --");

  ovElapsed = mkLabel(overlay, 0, 158, CFG_LCD_WIDTH, 10,
                      &lv_font_montserrat_8, C_FAINT);
  lv_obj_set_style_text_letter_space(ovElapsed, 1, LV_PART_MAIN);
  lv_label_set_text(ovElapsed, "ELAPSED 0s");

  ovHint = mkLabel(overlay, 0, 180, CFG_LCD_WIDTH, 10,
                   &lv_font_montserrat_8, C_FAINT);
  lv_obj_set_style_text_letter_space(ovHint, 1, LV_PART_MAIN);
  lv_label_set_text(ovHint, "MAKE SURE THE CAR IS AWAKE");
}

// ---------------------------------------------------------------------------
// 配钥引导卡片：真车不发提示、必须贴卡才弹确认框，因此屏幕直接把「现在贴卡」
// 和车端刷卡窗口倒计时显示出来，用户不用盯串口日志。
// ---------------------------------------------------------------------------
void Dashboard::buildPairCard() {
  pairCard = mkObj(lv_screen_active(), 0, 24, CFG_LCD_WIDTH, 216);
  lv_obj_set_style_bg_color(pairCard, lv_color_hex(C_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(pairCard, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_add_flag(pairCard, LV_OBJ_FLAG_HIDDEN);

  pairTitle = mkLabel(pairCard, 0, 8, CFG_LCD_WIDTH, 30,
                      &lv_font_montserrat_26, C_FG);
  lv_label_set_text(pairTitle, "ADD NEW KEY");

  pairStep = mkLabel(pairCard, 0, 42, CFG_LCD_WIDTH, 30,
                     &lv_font_montserrat_26, C_CAUTION);
  lv_label_set_text(pairStep, "TAP CARD");

  // 一行说明（位置/下一步动作），16 号字保证 320px 宽内不折行
  pairWhere = mkLabel(pairCard, 0, 76, CFG_LCD_WIDTH, 18,
                      &lv_font_montserrat_16, C_DIM);
  lv_label_set_text(pairWhere, "ON CONSOLE READER BY ARMREST");

  // 剩余秒数大字（车端刷卡窗口 ~30s）
  pairCount = mkLabel(pairCard, 0, 96, CFG_LCD_WIDTH, 52,
                      &lv_font_montserrat_48, C_FG);
  lv_label_set_text(pairCount, "30");

  // 倒计时进度条（手写两条矩形，LV_USE_BAR 为省 Flash 已关闭）
  pairBarTrack = mkObj(pairCard, 40, 152, 240, 8);
  lv_obj_set_style_radius(pairBarTrack, 4, LV_PART_MAIN);
  lv_obj_set_style_bg_color(pairBarTrack, lv_color_hex(C_TRACK), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(pairBarTrack, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(pairBarTrack, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(pairBarTrack, lv_color_hex(C_TRACK_BRD),
                                LV_PART_MAIN);

  pairBarFill = mkObj(pairCard, 40, 152, 240, 8);
  lv_obj_set_style_radius(pairBarFill, 4, LV_PART_MAIN);
  lv_obj_set_style_bg_color(pairBarFill, lv_color_hex(C_CAUTION),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(pairBarFill, LV_OPA_COVER, LV_PART_MAIN);

  pairAttempt = mkLabel(pairCard, 0, 164, CFG_LCD_WIDTH, 16,
                        &lv_font_montserrat_14, C_DIM);
  lv_label_set_text(pairAttempt, "TRY 1/3");

  pairStatus = mkLabel(pairCard, 0, 184, CFG_LCD_WIDTH, 20,
                       &lv_font_montserrat_16, C_CAUTION);
  lv_label_set_text(pairStatus, "WAITING FOR KEY CARD");
}

// ---------------------------------------------------------------------------
// 数据刷新（render 每帧调用；内部节流 + 只改脏属性）
// ---------------------------------------------------------------------------
void Dashboard::render(const VehicleData& v, BleLink::State bleState,
                       bool dataReady, bool handshakeOk,
                       const PairingHint& pair) {
  const uint32_t now = millis();
  if (now - m_lastPrint < CFG_RENDER_INTERVAL_MS) return;
  m_lastPrint = now;

  const bool prevOk = m_ok;
  // 遮罩只反映「链路/握手进度」，不再依赖数据是否刷新：
  //   车辆静止时车速恒为 0，若拿车速新鲜度判断会永远卡在 SYNCING。
  // 单字段缺数据时由 updateHero/updateBattery 各自降级显示 "--"。
  // 握手未完成仍显示遮罩，避免"没握手就显示数据"的假象。
  const bool ok = (bleState == BleLink::State::Ready) && handshakeOk;
  const bool statusChanged = bleState != m_bleState || ok != prevOk ||
                             pair.phase != m_pair.phase;

  m_v = v;
  if (bleState != m_bleState) {
    m_stateSince = now;      // overlay 的“当前状态已持续秒数”重新计时
    m_elapsedShown = 0xFFFFFFFF;  // 强制立刻刷新秒数
  }
  m_bleState = bleState;
  m_ok = ok;
  m_pair = pair;
  // 诊断日志：只在状态切换时打印一次，便于现场定位
  if (prevOk != ok) {
    CFG_LOG("[UI] 仪表状态变化: %s -> %s (ready=%s handshake=%s updatedAt=%ums valid=0x%04x)",
            prevOk ? "正常" : "SYNCING", ok ? "正常" : "SYNCING",
            dataReady ? "是" : "否", handshakeOk ? "是" : "否",
            millis() - v.updatedAt, v.valid);
  }
  // 配钥进行中（Idle 以外）也要持续刷新，否则倒计时会卡住
  if (!m_ok && m_hasRendered && !statusChanged &&
      pair.phase == PairingHint::Idle) {
    return;
  }

  updateAll();
  printSerial();
  m_hasRendered = true;
}

void Dashboard::updateAll() {
  updateHeader();
  updateCar();
  updateHero();
  updateBattery();
  updateOverlay();

  // 配钥提示优先于断连遮罩：两者都在同一块区域，只显示更相关的那个
  const bool showPair = pairCardVisible();
  updatePairCard();
  if (!m_ok && !showPair) {
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
  }
  // 断连时强制清空告警条（DESIGN.md §5）
  if (!m_ok) lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN);
}

bool Dashboard::pairCardVisible() const {
  if (m_pair.phase == PairingHint::Idle) return false;
  // 只在本机连上车端时提示配钥；链路掉了先显示断连遮罩
  return m_bleState == BleLink::State::Ready;
}

void Dashboard::updatePairCard() {
  const bool show = pairCardVisible();
  if (!show) {
    lv_obj_add_flag(pairCard, LV_OBJ_FLAG_HIDDEN);
    m_pairShownSec = 0xFFFFFFFF;  // 下次显示时强制重画倒计时
    return;
  }
  lv_obj_clear_flag(pairCard, LV_OBJ_FLAG_HIDDEN);

  const char* step = "";
  const char* detail = "";
  char detailBuf[40] = "";
  const char* status = "";
  uint32_t stepColor = C_CAUTION;
  uint32_t barColor = C_CAUTION;
  bool counting = false;

  switch (m_pair.phase) {
    case PairingHint::TapCard:
      step = "TAP CARD";
      detail = "ON CONSOLE READER BY ARMREST";
      status = "WAITING FOR KEY CARD";
      counting = true;
      break;
    case PairingHint::WaitConfirm:
      step = "CONFIRM";
      detail = "TAP YES ON THE TOUCHSCREEN";
      status = "CARD READ - CHECK SCREEN";
      stepColor = C_REGEN;
      barColor = C_REGEN;
      counting = true;
      break;
    case PairingHint::Retry:
      step = "TIMEOUT";
      detail = "TAP CARD ON NEXT ATTEMPT";
      status = "RETRYING IN A FEW SECONDS";
      break;
    case PairingHint::Success:
      step = "KEY ADDED";
      detail = "PUBLIC KEY SAVED";
      status = "PAIRED - RECONNECTING";
      stepColor = C_REGEN;
      barColor = C_REGEN;
      break;
    case PairingHint::Failed:
      step = "FAILED";
      snprintf(detailBuf, sizeof(detailBuf), "ERROR %u - CHECK SERIAL LOG",
               (unsigned)m_pair.info);
      detail = detailBuf;
      status = "PAIRING STOPPED";
      stepColor = C_ALERT;
      barColor = C_ALERT;
      break;
    default:
      break;
  }

  // 车端刷卡窗口已过：继续挂着「请贴卡」会误导，改成提示即将重试
  // （车端也不回 WAIT 时只能靠这个倒计时判断时机，不能用日志）
  if (counting && m_pair.remainMs == 0) {
    step = "WINDOW CLOSED";
    detail = "WILL RETRY AUTOMATICALLY";
    status = "WAITING TO RETRY...";
    stepColor = C_CAUTION;
    barColor = C_CAUTION;
    counting = false;
  }

  setText(pairStep, step);
  setTextColorC(pairStep, stepColor);
  setText(pairWhere, detail);
  setText(pairStatus, status);
  setTextColorC(pairStatus, stepColor);
  setBgColorC(pairBarFill, barColor);

  // 倒计时秒数（只在整秒变化时写，避免整屏重绘）
  const uint32_t sec = counting ? (m_pair.remainMs + 999) / 1000 : 0;
  if (sec != m_pairShownSec) {
    m_pairShownSec = sec;
    char buf[8];
    if (counting) {
      snprintf(buf, sizeof(buf), "%u", (unsigned)sec);
    } else {
      snprintf(buf, sizeof(buf), "--");
    }
    setText(pairCount, buf);
    setTextColorC(pairCount, sec > 0 || !counting ? C_FG : C_ALERT);
  }

  // 进度条：按「车端刷卡窗口」满量程
  int32_t w = 0;
  if (counting) {
    float frac = (float)m_pair.remainMs / (float)CFG_TESLA_PAIR_TAP_WINDOW_MS;
    if (frac > 1.0f) frac = 1.0f;
    w = (int32_t)(frac * 240.0f);
  }
  if (lv_obj_get_width(pairBarFill) != w) lv_obj_set_width(pairBarFill, w);

  // 第几次尝试
  char abuf[24];
  if (m_pair.maxAttempts > 0) {
    snprintf(abuf, sizeof(abuf), "TRY %u/%u", (unsigned)m_pair.attempt,
             (unsigned)m_pair.maxAttempts);
  } else {
    snprintf(abuf, sizeof(abuf), " ");
  }
  setText(pairAttempt, abuf);
}

void Dashboard::updateHeader() {
  // 档位高亮
  for (int i = 0; i < 4; ++i) {
    const bool on = m_ok && m_v.gear == "PRND"[i];
    setTextColorC(gearLbl[i], on ? C_FG : C_GEAR_OFF);
  }

  char buf[16];
  if (m_ok) {
    snprintf(buf, sizeof(buf), "%.1f°", m_v.outsideTempC);
    setText(outVal, buf);
    snprintf(buf, sizeof(buf), "%.1f°", m_v.insideTempC);
    setText(inVal, buf);
    setText(lockLbl, m_v.locked ? "LOCK" : "UNLK");
    setTextColorC(lockLbl, m_v.locked ? C_DIM : C_CAUTION);
  } else {
    setText(outVal, "--.-");
    setText(inVal, "--.-");
    setText(lockLbl, "UNLK");
  }

  // BLE 状态：已连接绿色、扫描/连接/重连琥珀、未启动灰、配钥中 KEY
  const bool pairing = pairCardVisible();
  setText(bleLbl, pairing ? "KEY" : bleStateText(m_bleState));
  uint32_t c = C_FAINT;
  if (pairing) {
    c = C_CAUTION;
  } else if (m_bleState == BleLink::State::Ready) {
    c = C_REGEN;
  } else if (m_bleState == BleLink::State::Scanning ||
             m_bleState == BleLink::State::Connecting ||
             m_bleState == BleLink::State::Reconnecting) {
    c = C_CAUTION;
  }
  setTextColorC(bleLbl, c);
}

void Dashboard::updateCar() {
  // 6 开口：关闭=暗色区块，打开=红色（闪烁由 onTimer 处理）
  for (int i = 0; i < 6; ++i) {
    const bool open = m_ok && m_v.doors[i];
    m_doorOpen[i] = open;
    setBgColorC(doorObj[i], open ? C_ALERT : C_DOOR_OFF);
    lv_obj_set_style_bg_opa(
        doorObj[i], open && !m_blinkOn ? LV_OPA_30 : LV_OPA_COVER,
        LV_PART_MAIN);
  }

  // 四轮着色 + 胎压数值 + 迷你量程条（1.5–3.5 bar 映射到 20px）
  for (int i = 0; i < 4; ++i) {
    const float bar = m_ok ? m_v.tpms[i] : 0.0f;
    const bool bad = m_ok && (bar < TPMS_LO || bar > TPMS_HI);
    uint32_t wheelColor = C_TPMS_OK;
    if (!m_ok) wheelColor = C_OFFLINE;
    else if (bad) wheelColor = C_ALERT;
    setBgColorC(wheelObj[i], wheelColor);

    int32_t fillW = 0;
    if (m_ok) {
      float frac = (bar - 1.5f) / 2.0f;  // 1.5..3.5 bar
      if (frac < 0.0f) frac = 0.0f;
      if (frac > 1.0f) frac = 1.0f;
      fillW = (int32_t)(frac * kTpmsBarW);

      char buf[8];
      snprintf(buf, sizeof(buf), "%.1f", bar);
      setText(tpmsVal[i], buf);
      setTextColorC(tpmsVal[i], bad ? C_ALERT : C_FG);
      setTextColorC(tpmsName[i], bad ? C_ALERT : C_FAINT);
      setBgColorC(tpmsBar[i], bad ? C_ALERT : C_TPMS_OK);
    } else {
      setText(tpmsVal[i], "--");
      setTextColorC(tpmsVal[i], C_OFFLINE);
      setTextColorC(tpmsName[i], C_OFFLINE);
      setBgColorC(tpmsBar[i], C_OFFLINE);
    }
    if (lv_obj_get_width(tpmsBar[i]) != fillW) {
      lv_obj_set_width(tpmsBar[i], fillW);
    }
  }

  // 告警条：车门 > 胎压 > 低电（同一时刻只 1 条）
  const char* text = nullptr;
  uint32_t color = C_ALERT;
  if (m_ok) {
    for (int i = 0; i < 6; ++i) {
      if (m_v.doors[i]) {
        static const char* const names[6] = {"FL DOOR OPEN",
                                             "FR DOOR OPEN",
                                             "RL DOOR OPEN",
                                             "RR DOOR OPEN",
                                             "FRUNK OPEN",
                                             "TRUNK OPEN"};
        text = names[i];
        break;
      }
    }
    if (!text) {
      static const char* const tn[4] = {"FL", "FR", "RL", "RR"};
      static char buf[32];
      for (int i = 0; i < 4; ++i) {
        const float bar = m_v.tpms[i];
        if (bar < TPMS_LO || bar > TPMS_HI) {
          snprintf(buf, sizeof(buf), "%s TIRE %.2f bar", tn[i], bar);
          text = buf;
          break;
        }
      }
    }
    if (!text && m_v.soc <= 15) {
      static char buf2[32];
      snprintf(buf2, sizeof(buf2), "LOW BATTERY · %.0f km RANGE",
               m_v.rangeKm);
      text = buf2;
      color = C_CAUTION;
    }
  }

  if (text) {
    char bbuf[56];
    snprintf(bbuf, sizeof(bbuf), "%s %s", LV_SYMBOL_WARNING, text);
    setBgColorC(banner, color);
    setText(bannerLbl, bbuf);
    lv_obj_clear_flag(banner, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN);
  }
}

void Dashboard::updateHero() {
  // 充电模式：hero 扩展全宽 x104-320，隐藏电量列（DESIGN.md §3）
  const bool chg = m_ok && m_v.charging;
  if (chg != m_chargingLayout) {
    setChargingLayout(chg);
  }

  if (chg) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f kW", m_v.chargePowerKw);
    setText(chargeKwLbl, buf);
    // 功率柱：0-250kW 满量程 → 110px 高（从底部长）
    float frac = m_v.chargePowerKw / 250.0f;
    if (frac > 1.0f) frac = 1.0f;
    const int32_t fh = (int32_t)(frac * 110.0f);
    setHeightIfChanged(chargeFill, fh);
    setPosIfChanged(chargeFill, 196, 20 + (110 - fh));
    // SOC 进度条宽 = SOC%（188px 满宽）
    if (lv_obj_get_width(chargeSocBar) != (int32_t)(m_v.soc * 188 / 100)) {
      lv_obj_set_width(chargeSocBar, (int32_t)(m_v.soc * 188 / 100));
    }
    // 简易 ETA：剩余 kWh 按 75kWh 电池包估算（演示用）
    const float remainKwh = (100 - m_v.soc) * 0.75f;
    const float hours =
        m_v.chargePowerKw > 0.1f ? remainKwh / m_v.chargePowerKw : 0;
    snprintf(buf, sizeof(buf), "EST REMAINING %dH %02dM", (int)hours,
             (int)((hours - (int)hours) * 60));
    setText(chargeEtaLbl, buf);
  } else if (m_ok) {
    // 补间后的车速（m_spdShown 由 onTimer 每 50ms 插值）
    char buf[8];
    snprintf(buf, sizeof(buf), "%.0f", m_spdShown);
    setText(spdLbl, buf);
  } else {
    setText(spdLbl, "--");
  }
}

void Dashboard::setChargingLayout(bool charging) {
  m_chargingLayout = charging;
  if (charging) {
    lv_obj_set_width(hero, 216);  // x104-320 全宽
    lv_obj_add_flag(spdLbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(spdUnit, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(colBatt, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(chargePanel, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_set_width(hero, 146);  // 回到 x104-250
    lv_obj_clear_flag(spdLbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(spdUnit, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(colBatt, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(chargePanel, LV_OBJ_FLAG_HIDDEN);
  }
}

void Dashboard::updateBattery() {
  char buf[16];
  if (m_ok) {
    snprintf(buf, sizeof(buf), "%ld", (long)m_v.soc);
    setText(socLbl, buf);
    snprintf(buf, sizeof(buf), "%.0f km", m_v.rangeKm);
    setText(rangeLbl, buf);

    // 电量条：底部向上生长（y = 46 + 110 - h, 高 = SOC% × 110）
    const int32_t fh = (int32_t)(m_v.soc * 110 / 100);
    setHeightIfChanged(battFill, fh);
    setPosIfChanged(battFill, 28, 46 + 110 - fh);
    uint32_t c = C_FG;
    if (m_v.charging) c = C_REGEN;
    else if (m_v.soc <= 15) c = C_ALERT;
    setBgColorC(battFill, c);
    setTextColorC(socLbl, m_v.soc <= 15 ? C_ALERT : C_FG);
    setTextColorC(socPct, m_v.soc <= 15 ? C_ALERT : C_DIM);
  } else {
    setText(socLbl, "--");
    setText(rangeLbl, "-- km");
    setHeightIfChanged(battFill, 0);
    setTextColorC(socLbl, C_FG);
    setTextColorC(socPct, C_DIM);
  }
}

void Dashboard::updateOverlay() {
  // 状态大字 / 说明 / 提示 / 强调色 / 当前阶段：与主数据页同一套风格，ASCII
  const char* title = "SCANNING";
  const char* sub = "SEARCHING FOR TESLA BROADCAST";
  const char* hint = "MAKE SURE THE CAR IS AWAKE";
  uint32_t accent = C_CAUTION;
  int stage = 0;

  switch (m_bleState) {
    case BleLink::State::Idle:
      title = "BLE IDLE";
      sub = "STACK NOT STARTED";
      hint = "CHECK SERIAL LOG";
      accent = C_FAINT;
      stage = 0;
      break;
    case BleLink::State::Scanning:
      title = "SCANNING";
      sub = "SEARCHING FOR TESLA BROADCAST";
      hint = "MAKE SURE THE CAR IS AWAKE";
      accent = C_CAUTION;
      stage = 0;
      break;
    case BleLink::State::Connecting:
      title = "CONNECTING";
      sub = "OPENING BLE LINK TO VEHICLE";
      hint = "STAY WITHIN A FEW METERS";
      accent = C_CAUTION;
      stage = 1;
      break;
    case BleLink::State::Ready:
      // 链路已通但数据/握手未就绪
      title = "SYNCING";
      sub = "WAITING FOR VEHICLE DATA";
      hint = "HANDSHAKE IN PROGRESS";
      accent = C_FG;
      stage = 2;
      break;
    case BleLink::State::Reconnecting:
      title = "RECONNECTING";
      sub = "LINK LOST - RETRYING";
      hint = "MOVE CLOSER TO THE CAR";
      accent = C_ALERT;
      stage = 1;
      break;
  }

  setText(ovTitle, title);
  setTextColorC(ovTitle, C_FG);
  setText(ovSub, sub);
  setTextColorC(ovSub, C_DIM);
  setText(ovHint, hint);
  setTextColorC(ovCap, accent);
  // 三点脉冲强调色随状态（Idle/Scan 琥珀，Conn 琥珀，Ready 白，Reconn 红）
  for (int i = 0; i < 3; ++i) setBgColorC(dots[i], accent);

  // 阶段面包屑：完成=绿，当前=强调色，未到=暗灰
  for (int i = 0; i < 3; ++i) {
    uint32_t c;
    if (i < stage) c = C_REGEN;
    else if (i == stage) c = accent;
    else c = C_GEAR_OFF;
    setTextColorC(ovStep[i], c);
  }

  // 目标 VIN（config.h 提供，空则提示未设置）
  char buf[40];
  const char* vin = CFG_TESLA_VIN;
  snprintf(buf, sizeof(buf), "VIN %s", (vin != nullptr && vin[0]) ? vin : "--");
  setText(ovTarget, buf);
  setTextColorC(ovTarget, accent);

  // 当前状态已持续秒数：状态切换时 render 会重置 m_stateSince
  const uint32_t sec = (millis() - m_stateSince) / 1000;
  if (sec != m_elapsedShown) {
    m_elapsedShown = sec;
    snprintf(buf, sizeof(buf), "ELAPSED %lus", (unsigned long)sec);
    setText(ovElapsed, buf);
  }
  setTextColorC(ovElapsed, accent);
}

// ---------------------------------------------------------------------------
// 50ms 动画定时器：车速补间 + 1Hz 告警闪烁 + 断连三点脉冲
// ---------------------------------------------------------------------------
void Dashboard::timerCb(lv_timer_t* t) {
  // LVGL 9 的 lv_timer_t 是不透明类型，用户数据只能经 getter 获取
  static_cast<Dashboard*>(lv_timer_get_user_data(t))->onTimer();
}

void Dashboard::onTimer() {
  ++m_tick;

  // 车速 200ms ease-out 补间（DESIGN.md §6）
  if (m_ok && !m_chargingLayout) {
    const float target = m_v.speedKmh;
    m_spdShown += (target - m_spdShown) * 0.35f;
    if (target - m_spdShown < 0.5f && m_spdShown - target < 0.5f) {
      m_spdShown = target;
    }
    // 只在数字变化时才写（每 50ms 无条件 set_text 会造成整屏重绘闪动）
    char buf[8];
    snprintf(buf, sizeof(buf), "%.0f", m_spdShown);
    setText(spdLbl, buf);
  } else {
    m_spdShown = 0;
  }

  // 1Hz 方波闪烁（500ms 翻转）：开口块 + 告警条
  if (m_tick % 10 == 0) {
    m_blinkOn = !m_blinkOn;
    const lv_opa_t opa = m_blinkOn ? LV_OPA_COVER : LV_OPA_30;
    for (int i = 0; i < 6; ++i) {
      if (m_doorOpen[i]) {
        lv_obj_set_style_bg_opa(doorObj[i], opa, LV_PART_MAIN);
      }
    }
    if (!lv_obj_has_flag(banner, LV_OBJ_FLAG_HIDDEN)) {
      lv_obj_set_style_bg_opa(banner, m_blinkOn ? LV_OPA_60 : LV_OPA_20,
                              LV_PART_MAIN);
    }
    // 异常胎压数值同步闪烁
    if (m_ok) {
      for (int i = 0; i < 4; ++i) {
        const float bar = m_v.tpms[i];
        if (bar < TPMS_LO || bar > TPMS_HI) {
          lv_obj_set_style_text_color(
              tpmsVal[i],
              lv_color_hex(m_blinkOn ? C_ALERT : C_CAUTION),
              LV_PART_MAIN);
        }
      }
    }
  }

  // 断连三点错相脉冲（600ms 周期 / 相差 200ms）
  if (!lv_obj_has_flag(overlay, LV_OBJ_FLAG_HIDDEN)) {
    for (int i = 0; i < 3; ++i) {
      const int phase = (int)((m_tick / 12 + i) % 3);
      lv_obj_set_style_bg_opa(dots[i],
                              phase == 0 ? LV_OPA_80 : LV_OPA_20,
                              LV_PART_MAIN);
    }
  }
}

// ---------------------------------------------------------------------------
// 串口 echo（无屏也能验证数据链路）
// ---------------------------------------------------------------------------
void Dashboard::printSerial() {
  const VehicleData& v = m_v;
  char speed[12] = "--";
  char soc[8] = "--";
  char range[16] = "--";
  char gear[4] = "--";
  char charge[20] = "--";
  char tpms[32] = "--";

  if (m_ok) {
    snprintf(speed, sizeof(speed), "%.0f", v.speedKmh);
    snprintf(soc, sizeof(soc), "%ld%%", (long)v.soc);
    snprintf(range, sizeof(range), "%.0f km", v.rangeKm);
    gear[0] = v.gear;
    gear[1] = '\0';
    if (v.charging) {
      snprintf(charge, sizeof(charge), "%.1f kW", v.chargePowerKw);
    } else {
      snprintf(charge, sizeof(charge), "未充电");
    }
    snprintf(tpms, sizeof(tpms), "%.1f/%.1f/%.1f/%.1f", v.tpms[0],
             v.tpms[1], v.tpms[2], v.tpms[3]);
  }

  char output[sizeof(m_lastSerialOutput)];
  int written = snprintf(
      output, sizeof(output),
      "[仪表] BLE:%s | 数据:%s | 车速 %s km/h | SOC %s | 续航 %s | 档位 %s\r\n",
      bleStateText(m_bleState), m_ok ? "就绪" : "----", speed, soc, range,
      gear);
  if (written < 0 || (size_t)written >= sizeof(output)) {
    CFG_LOG("仪表串口状态格式化失败");
    return;
  }

  if (m_ok) {
    const int appended = snprintf(
        output + written, sizeof(output) - (size_t)written,
        "        充电:%s | 内%.1f 外%.1f | 胎压(FL/FR/RL/RR):%s\r\n", charge,
        v.insideTempC, v.outsideTempC, tpms);
    if (appended < 0 ||
        (size_t)appended >= sizeof(output) - (size_t)written) {
      CFG_LOG("仪表串口数据格式化失败");
      return;
    }
    written += appended;
  }

  if (m_serialPrinted && strcmp(output, m_lastSerialOutput) == 0) return;

  memcpy(m_lastSerialOutput, output, (size_t)written + 1);
  m_serialPrinted = true;
  Serial.print(output);
}
