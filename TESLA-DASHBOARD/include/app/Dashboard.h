#pragma once

#include <lvgl.h>

#include "ble/BleLink.h"
#include "data/VehicleData.h"

// ---------------------------------------------------------------------------
// Dashboard：仪表 UI 层（LVGL 9 对象树，布局见 dashboard/DESIGN.md）
//
//   hdr     顶部状态条：档位 · 内外温 · 锁/BLE
//   banner  告警条（车门 > 胎压 > 低电，1Hz 闪烁）
//   carcol  左栏：车身俯视 + 6 开口 + 四轮着色 + 胎压数值
//   hero    中区：车速大字；充电模式扩展全宽换充电面板
//   batt    右列：SOC + 电量条 + 续航（充电模式隐藏）
//   overlay 断连遮罩
//
// 数据只来自 g_vehicle（applyTesla 写入），BLE 与演示剧本同一路径。
// ---------------------------------------------------------------------------
class Dashboard {
 public:
  void begin();
  // handshakeOk：加密会话是否已握手成功；false 时一律按无数据渲染
  void render(const VehicleData& v, BleLink::State bleState, bool dataReady,
              bool handshakeOk);

 private:
  // LVGL 定时器：数值补间(50ms) + 告警 1Hz 闪烁 + 断连三点脉冲
  static void timerCb(lv_timer_t* t);
  void onTimer();

  void buildUi();
  void buildHeader();
  void buildBanner();
  void buildCarColumn();
  void buildHero();
  void buildBatteryColumn();
  void buildOverlay();

  void updateAll();
  void updateHeader();
  void updateCar();
  void updateHero();
  void updateBattery();
  void setChargingLayout(bool charging);
  void printSerial();

  // 数据快照副本（定时器动画需要，render 时刷新）
  VehicleData m_v;
  uint32_t m_lastPrint = 0;
  BleLink::State m_bleState = BleLink::State::Idle;
  bool m_ok = false;
  bool m_hasRendered = false;
  bool m_chargingLayout = false;
  bool m_blinkOn = true;        // 1Hz 方波相位
  uint32_t m_tick = 0;          // 50ms 定时器节拍计数
  float m_spdShown = 0;         // 补间中的显示车速

  // ---- 顶层容器 ----
  lv_obj_t* hdr = nullptr;
  lv_obj_t* banner = nullptr;
  lv_obj_t* bannerLbl = nullptr;
  lv_obj_t* carcol = nullptr;
  lv_obj_t* hero = nullptr;
  lv_obj_t* colBatt = nullptr;
  lv_obj_t* overlay = nullptr;

  // ---- hdr ----
  lv_obj_t* gearLbl[4] = {nullptr, nullptr, nullptr, nullptr};
  lv_obj_t* tempsLbl = nullptr;
  lv_obj_t* lockLbl = nullptr;
  lv_obj_t* bleLbl = nullptr;

  // ---- carcol ----
  lv_obj_t* doorObj[6] = {nullptr, nullptr, nullptr,
                          nullptr, nullptr, nullptr};
  lv_obj_t* wheelObj[4] = {nullptr, nullptr, nullptr, nullptr};
  lv_obj_t* tpmsVal[4] = {nullptr, nullptr, nullptr, nullptr};

  // ---- hero ----
  lv_obj_t* spdLbl = nullptr;
  lv_obj_t* chargePanel = nullptr;
  lv_obj_t* chargeKwLbl = nullptr;
  lv_obj_t* chargeTrack = nullptr;   // 功率柱槽
  lv_obj_t* chargeFill = nullptr;    // 功率柱填充
  lv_obj_t* chargeSocBar = nullptr;  // lv_bar 横向 SOC 进度
  lv_obj_t* chargeEtaLbl = nullptr;

  // ---- col-batt ----
  lv_obj_t* socLbl = nullptr;
  lv_obj_t* battTrack = nullptr;  // 竖向电量条槽
  lv_obj_t* battFill = nullptr;   // 竖向电量条填充
  lv_obj_t* rangeLbl = nullptr;

  // ---- overlay ----
  lv_obj_t* dots[3] = {nullptr, nullptr, nullptr};
};
