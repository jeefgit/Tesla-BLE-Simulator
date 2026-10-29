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
constexpr int32_t CAR_Y = 40;
constexpr int32_t CAR_W = 52;
constexpr int32_t CAR_H = 120;

// 6 开口高亮位置（相对 carcol）：顶前盖 / 左前 / 右前 / 左后 / 右后 / 尾箱
// 与 TeslaPayload doors[6] 顺序 fl,fr,rl,rr,frunk,trunk 一一对应
const int32_t kDoorXY[6][4] = {
    {CAR_X + 2, CAR_Y + 26, 5, 30},          // FL 左前门
    {CAR_X + CAR_W - 7, CAR_Y + 26, 5, 30},  // FR 右前门
    {CAR_X + 2, CAR_Y + 64, 5, 28},          // RL 左后门
    {CAR_X + CAR_W - 7, CAR_Y + 64, 5, 28},  // RR 右后门
    {CAR_X + 12, CAR_Y + 2, CAR_W - 24, 6},  // FRUNK 前盖
    {CAR_X + 12, CAR_Y + CAR_H - 8, CAR_W - 24, 6},  // TRUNK 尾箱
};

// 4 车轮位置（相对 carcol）：与 tpms[4] fl,fr,rl,rr 对应
const int32_t kWheelXY[4][4] = {
    {CAR_X - 6, CAR_Y + 28, 6, 24},                    // FL
    {CAR_X + CAR_W, CAR_Y + 28, 6, 24},                // FR
    {CAR_X - 6, CAR_Y + 66, 6, 24},                    // RL
    {CAR_X + CAR_W, CAR_Y + 66, 6, 24},                // RR
};

// 胎压数值块：各轮外侧（FL/RL 贴左缘右对齐，FR/RR 贴右缘左对齐）
const int32_t kTpmsXY[4][4] = {
    {0, CAR_Y + 26, 22, 28},                  // FL 左外
    {CAR_W + 12, CAR_Y + 26, 22, 28},         // FR 右外
    {0, CAR_Y + 64, 22, 28},                  // RL 左外
    {CAR_W + 12, CAR_Y + 64, 22, 28},         // RR 右外
};
}  // namespace

// ---------------------------------------------------------------------------
// 构建静态对象树（只建一次，之后 updateAll 只改属性）
// ---------------------------------------------------------------------------
void Dashboard::begin() {
  buildUi();
  lv_timer_create(&Dashboard::timerCb, 50, this);  // 50ms 动画节拍
  CFG_LOG("仪表层就绪（LVGL 对象树, %dx%d）", CFG_LCD_WIDTH,
          CFG_LCD_HEIGHT);
}

void Dashboard::buildUi() {
  lv_obj_t* scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), LV_PART_MAIN);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  buildHeader();     // y0-24
  buildCarColumn();  // x0-104, y24-240
  buildHero();       // x104-250, y24-240（充电时扩展到 x320）
  buildBatteryColumn();  // x250-320, y24-240（充电时隐藏）
  buildBanner();     // y24-44 覆盖式（最后建保证 z 序在车身上方）
  buildOverlay();    // 断连遮罩最顶层
}

void Dashboard::buildHeader() {
  hdr = mkObj(lv_screen_active(), 0, 0, CFG_LCD_WIDTH, 24);
  // 底边 1px 分割线
  lv_obj_set_style_border_side(hdr, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
  lv_obj_set_style_border_width(hdr, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(hdr, lv_color_hex(C_LINE), LV_PART_MAIN);
  lv_obj_set_style_bg_color(hdr, lv_color_hex(C_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, LV_PART_MAIN);

  // 档位 P R N D（左对齐一排）
  const char gears[4] = {'P', 'R', 'N', 'D'};
  for (int i = 0; i < 4; ++i) {
    gearLbl[i] = mkLabel(hdr, 8 + i * 20, 3, 18, 18,
                         &lv_font_montserrat_16, C_GEAR_OFF);
    lv_label_set_text_fmt(gearLbl[i], "%c", gears[i]);
  }

  // 内外温（居中）
  tempsLbl = mkLabel(hdr, 90, 4, 140, 16, &lv_font_montserrat_16, C_DIM);
  lv_label_set_text(tempsLbl, "OUT --.-  IN --.-");

  // 锁 + BLE 状态（右侧）
  lockLbl = mkLabel(hdr, 240, 4, 34, 16, &lv_font_montserrat_16, C_DIM,
                    LV_TEXT_ALIGN_RIGHT);
  lv_label_set_text(lockLbl, "UNLK");
  bleLbl = mkLabel(hdr, 278, 4, 38, 16, &lv_font_montserrat_14, C_FAINT,
                   LV_TEXT_ALIGN_RIGHT);
  lv_label_set_text(bleLbl, "--");
}

void Dashboard::buildBanner() {
  banner = mkObj(lv_screen_active(), 0, 24, CFG_LCD_WIDTH, 20);
  lv_obj_set_style_bg_color(banner, lv_color_hex(C_ALERT), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(banner, LV_OPA_60, LV_PART_MAIN);
  lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN);

  bannerLbl = mkLabel(banner, 0, 2, CFG_LCD_WIDTH, 16,
                      &lv_font_montserrat_16, C_FG);
  lv_label_set_text(bannerLbl, "");
}

void Dashboard::buildCarColumn() {
  carcol = mkObj(lv_screen_active(), 0, 24, 104, 216);
  // 右边框 1px 分割
  lv_obj_set_style_border_side(carcol, LV_BORDER_SIDE_RIGHT, LV_PART_MAIN);
  lv_obj_set_style_border_width(carcol, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(carcol, lv_color_hex(C_LINE),
                                LV_PART_MAIN);

  // 标题
  lv_obj_t* cap = mkLabel(carcol, 0, 4, 104, 12, &lv_font_montserrat_14,
                          C_FAINT);
  lv_label_set_text(cap, "TPMS · BAR");

  // 车身轮廓（圆角矩形，车头朝上）
  lv_obj_t* body = mkObj(carcol, CAR_X, CAR_Y, CAR_W, CAR_H);
  lv_obj_set_style_radius(body, 14, LV_PART_MAIN);
  lv_obj_set_style_bg_color(body, lv_color_hex(C_TRACK), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(body, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(body, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(body, lv_color_hex(C_TRACK_BRD),
                                LV_PART_MAIN);
  // 车顶中线（1px 竖线，示意轴线）
  lv_obj_t* spine = mkObj(body, CAR_W / 2, 8, 1, CAR_H - 16);
  lv_obj_set_style_bg_color(spine, lv_color_hex(C_LINE), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(spine, LV_OPA_COVER, LV_PART_MAIN);

  // 6 开口高亮块（默认隐藏 = 全关）
  for (int i = 0; i < 6; ++i) {
    doorObj[i] = mkObj(carcol, kDoorXY[i][0], kDoorXY[i][1],
                       kDoorXY[i][2], kDoorXY[i][3]);
    lv_obj_set_style_radius(doorObj[i], 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(doorObj[i], lv_color_hex(C_ALERT),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(doorObj[i], LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_flag(doorObj[i], LV_OBJ_FLAG_HIDDEN);
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

  // 4 胎压数值（车轮外侧，数值 + 位号）
  for (int i = 0; i < 4; ++i) {
    tpmsVal[i] = mkLabel(carcol, kTpmsXY[i][0], kTpmsXY[i][1],
                         kTpmsXY[i][2], kTpmsXY[i][3],
                         &lv_font_montserrat_14, C_FG,
                         i % 2 == 0 ? LV_TEXT_ALIGN_RIGHT
                                    : LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(tpmsVal[i], "--");
  }
}

void Dashboard::buildHero() {
  hero = mkObj(lv_screen_active(), 104, 24, 146, 216);

  // 车速大字
  spdLbl = mkLabel(hero, 0, 50, 146, 64, &lv_font_montserrat_48, C_FG);
  lv_label_set_text(spdLbl, "--");

  lv_obj_t* unit = mkLabel(hero, 0, 118, 146, 18, &lv_font_montserrat_16,
                           C_DIM);
  lv_label_set_text(unit, "km/h");

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

  // SOC 数值
  socLbl = mkLabel(colBatt, 0, 8, 70, 30, &lv_font_montserrat_26, C_FG);
  lv_label_set_text(socLbl, "--");

  // 竖向电量条（10×68 胶囊，y36-104）
  battTrack = mkObj(colBatt, 30, 40, 10, 68);
  lv_obj_set_style_radius(battTrack, 5, LV_PART_MAIN);
  lv_obj_set_style_bg_color(battTrack, lv_color_hex(C_TRACK),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(battTrack, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(battTrack, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(battTrack, lv_color_hex(C_TRACK_BRD),
                                LV_PART_MAIN);
  // 填充从底部往上长（自底对齐，高度随 SOC）
  battFill = mkObj(colBatt, 30, 108, 10, 0);
  lv_obj_set_style_radius(battFill, 5, LV_PART_MAIN);
  lv_obj_set_style_bg_color(battFill, lv_color_hex(C_FG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(battFill, LV_OPA_COVER, LV_PART_MAIN);

  // 续航
  rangeLbl = mkLabel(colBatt, 0, 116, 70, 20, &lv_font_montserrat_16,
                     C_FG);
  lv_label_set_text(rangeLbl, "--");

  lv_obj_t* cap = mkLabel(colBatt, 0, 140, 70, 12,
                          &lv_font_montserrat_14, C_FAINT);
  lv_label_set_text(cap, "RANGE");
}

void Dashboard::buildOverlay() {
  overlay = mkObj(lv_screen_active(), 0, 24, CFG_LCD_WIDTH, 216);
  lv_obj_set_style_bg_color(overlay, lv_color_hex(C_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_80, LV_PART_MAIN);
  lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t* msg = mkLabel(overlay, 0, 92, CFG_LCD_WIDTH, 20,
                          &lv_font_montserrat_16, C_DIM);
  lv_label_set_text(msg, "Waiting for Tesla BLE...");

  // 三点脉冲（错相 0.2s）
  for (int i = 0; i < 3; ++i) {
    dots[i] = mkObj(overlay, CFG_LCD_WIDTH / 2 - 20 + i * 14, 124, 6, 6);
    lv_obj_set_style_radius(dots[i], 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dots[i], lv_color_hex(C_DIM),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dots[i], LV_OPA_40, LV_PART_MAIN);
  }
}

// ---------------------------------------------------------------------------
// 数据刷新（render 每帧调用；内部节流 + 只改脏属性）
// ---------------------------------------------------------------------------
void Dashboard::render(const VehicleData& v, BleLink::State bleState,
                       bool dataReady) {
  const uint32_t now = millis();
  if (now - m_lastPrint < CFG_RENDER_INTERVAL_MS) return;
  m_lastPrint = now;

  m_v = v;
  m_bleState = bleState;
  m_ok = dataReady && v.fresh(VF_SPEED, CFG_DATA_STALE_MS);

  updateAll();
  printSerial();
}

void Dashboard::updateAll() {
  updateHeader();
  updateCar();
  updateHero();
  updateBattery();

  lv_obj_add_flag(overlay, m_ok ? LV_OBJ_FLAG_HIDDEN : LV_OBJ_FLAG_HIDDEN);
  if (!m_ok) lv_obj_clear_flag(overlay, LV_OBJ_FLAG_HIDDEN);
  // 断连时强制清空告警条（DESIGN.md §5）
  if (!m_ok) lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN);
}

void Dashboard::updateHeader() {
  // 档位高亮
  for (int i = 0; i < 4; ++i) {
    const bool on = m_ok && m_v.gear == "PRND"[i];
    setTextColorC(gearLbl[i], on ? C_FG : C_GEAR_OFF);
  }

  char buf[32];
  if (m_ok) {
    snprintf(buf, sizeof(buf), "OUT %.1f  IN %.1f", m_v.outsideTempC,
             m_v.insideTempC);
    setText(tempsLbl, buf);
    setText(lockLbl, m_v.locked ? "LOCK" : "UNLK");
    setTextColorC(lockLbl, m_v.locked ? C_DIM : C_CAUTION);
  } else {
    setText(tempsLbl, "OUT --.-  IN --.-");
    setText(lockLbl, "UNLK");
  }
  setText(bleLbl, bleStateText(m_bleState));
}

void Dashboard::updateCar() {
  // 6 开口高亮（打开=红色块 + banner 提示，闪烁由 onTimer 处理）
  for (int i = 0; i < 6; ++i) {
    const bool open = m_ok && m_v.doors[i];
    if (open) {
      lv_obj_clear_flag(doorObj[i], LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(doorObj[i], LV_OBJ_FLAG_HIDDEN);
    }
  }

  // 四轮着色 + 胎压数值
  for (int i = 0; i < 4; ++i) {
    const float bar = m_ok ? m_v.tpms[i] : 0.0f;
    const bool bad = m_ok && (bar < TPMS_LO || bar > TPMS_HI);
    uint32_t wheelColor = C_TPMS_OK;
    if (!m_ok) wheelColor = C_OFFLINE;
    else if (bad) wheelColor = C_ALERT;
    setBgColorC(wheelObj[i], wheelColor);

    char buf[8];
    if (m_ok) {
      snprintf(buf, sizeof(buf), "%.1f", bar);
      setText(tpmsVal[i], buf);
      setTextColorC(tpmsVal[i], bad ? C_ALERT : C_FG);
    } else {
      setText(tpmsVal[i], "--");
      setTextColorC(tpmsVal[i], C_OFFLINE);
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
    setBgColorC(banner, color);
    setText(bannerLbl, text);
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
    lv_obj_add_flag(colBatt, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(chargePanel, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_set_width(hero, 146);  // 回到 x104-250
    lv_obj_clear_flag(spdLbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(colBatt, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(chargePanel, LV_OBJ_FLAG_HIDDEN);
  }
}

void Dashboard::updateBattery() {
  char buf[16];
  if (m_ok) {
    snprintf(buf, sizeof(buf), "%ld%%", (long)m_v.soc);
    setText(socLbl, buf);
    snprintf(buf, sizeof(buf), "%.0f km", m_v.rangeKm);
    setText(rangeLbl, buf);

    // 电量条：底部向上生长（y = 108 - h, 高 = SOC% × 68）
    const int32_t fh = (int32_t)(m_v.soc * 68 / 100);
    setHeightIfChanged(battFill, fh);
    setPosIfChanged(battFill, 30, 108 - fh);
    uint32_t c = C_FG;
    if (m_v.charging) c = C_REGEN;
    else if (m_v.soc <= 15) c = C_ALERT;
    setBgColorC(battFill, c);
    setTextColorC(socLbl, m_v.soc <= 15 ? C_ALERT : C_FG);
  } else {
    setText(socLbl, "--");
    setText(rangeLbl, "--");
    setHeightIfChanged(battFill, 0);
  }
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
      if (!lv_obj_has_flag(doorObj[i], LV_OBJ_FLAG_HIDDEN)) {
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

  Serial.printf(
      "[仪表] BLE:%s | 数据:%s | 车速 %s km/h | SOC %s | 续航 %s | 档位 %s\r\n",
      bleStateText(m_bleState), m_ok ? "就绪" : "----", speed, soc, range,
      gear);
  if (m_ok) {
    Serial.printf(
        "        充电:%s | 内%.1f 外%.1f | 胎压(FL/FR/RL/RR):%s\r\n",
        charge, v.insideTempC, v.outsideTempC, tpms);
  }
}
