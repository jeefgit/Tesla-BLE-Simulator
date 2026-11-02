#include "display/Display.h"

#include <LovyanGFX.hpp>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <lvgl.h>

#include "config.h"

// LVGL 断言处理器（lv_conf.h 里用 LV_ASSERT_HANDLER 指向这里）。
// 默认的 while(1) 会静默卡死，实车上看不出是内存不够还是代码写错；
// 这里打一行中文日志再重启，至少串口能留下线索。
extern "C" void lvglAssertFailed(const char* file, int line) {
  CFG_LOG("错误: LVGL 断言失败 (%s:%d) —— 内存不足或参数非法，重启",
          file, line);
  Serial.flush();
  delay(100);
  esp_restart();
}

// LVGL 对象池（lv_conf.h: LV_MEM_POOL_ALLOC = 这里，LV_MEM_SIZE = 64KB）。
// 必须 8 字节以上对齐：TLSF 会按 ALIGN_SIZE 检查，普通 malloc 在 ESP32 上
// 只保证 4 字节，对齐不够时 add/create 会失败。这里给 16 字节。
extern "C" void* lvglPoolAlloc(size_t size) {
  void* p = heap_caps_aligned_alloc(16, size, MALLOC_CAP_8BIT);
  if (p == nullptr) {
    CFG_LOG("错误: LVGL 池申请 %u B 失败（ESP 空闲 %u B / 最大块 %u B）",
            (unsigned)size, (unsigned)ESP.getFreeHeap(),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    Serial.flush();
    delay(100);
    esp_restart();  // 没有池，LVGL 后续必然写 NULL：直接重启等下次
  }
  return p;
}

// ---------------------------------------------------------------------------
// LovyanGFX 面板配置：2.8 寸 320×240 SPI
// 驱动芯片由 config.h 的 CFG_LCD_DRIVER_ILI9341 选择：
//   1 = ILI9341（ESP32-2432S028R / CYD 板载）
//   0 = ST7789（独立排线屏常见）
// 引脚全部来自 config.h，RST/BL 悬空填 -1。
//
// ⚠️ 此板屏幕调试点（换屏时先看这里）：
//   1. LCD 走 HSPI 14/13/12，18/23 是 TF 卡脚（接错则全白）
//   2. RST 与 MISO 共用 IO12
//   3. 克隆 ILI9341 按 320x240 声明才稳定（按 240x320 原生声明会花屏）
//   4. 屏幕水平镜像，用 offset_rotation=4 官方翻转位修正
//   5. TF 卡(IO5)/触摸(IO33) 与 LCD 共用 SPI 总线，启动先拉高片选
//
// 这套参数下驱动坐标是竖装 240×320；UI 的横屏方向由 flushCb 里的
// CFG_LCD_FLUSH_ROTATION 旋转完成，面板参数不要跟着改。
// ---------------------------------------------------------------------------
#if CFG_LCD_DRIVER_ILI9341
using PanelChip = lgfx::Panel_ILI9341;
#else
using PanelChip = lgfx::Panel_ST7789;
#endif

static_assert(CFG_LCD_FLUSH_ROTATION == 0 || CFG_LCD_FLUSH_ROTATION == 90 ||
                  CFG_LCD_FLUSH_ROTATION == 270,
              "CFG_LCD_FLUSH_ROTATION 只能是 0 / 90 / 270（180 放不下 320 宽）");

class DashboardPanel : public lgfx::LGFX_Device {
 public:
  DashboardPanel() {
    {
      auto cfg = _bus.config();            // SPI 总线
      cfg.pin_sclk = CFG_LCD_SCK;
      cfg.pin_mosi = CFG_LCD_MOSI;
      cfg.pin_miso = CFG_LCD_MISO;         // 与 RST 共用 IO12
      cfg.pin_dc = CFG_LCD_DC;
      cfg.spi_mode = 0;
      cfg.spi_host = SPI2_HOST;            // 显式 HSPI
      cfg.freq_write = CFG_LCD_SPI_FREQ;
      cfg.freq_read = 16000000;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();          // 面板
      cfg.pin_cs = CFG_LCD_CS;
      cfg.pin_rst = CFG_LCD_RST;
      cfg.panel_width = 320;
      cfg.panel_height = 240;
      cfg.memory_width = 320;
      cfg.memory_height = 240;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      // 水平镜像修正：rot1 + offset4 -> MADCTL 0xA8
      // （库在 setRotation 时自动重发，优于裸写会被缓存覆盖）
      cfg.offset_rotation = CFG_LCD_OFFSET_ROTATION;
      cfg.invert = CFG_LCD_INVERT;         // 1 = 反色（黑显示成白就是它错了）
      // 红蓝通道顺序：库内部 madctl |= (rgb_order ? MAD_RGB : MAD_BGR)
      // 默认 false 会写 MAD_BGR，导致红蓝互换（暗蓝黑 #0E1219 显示成棕红）
      cfg.rgb_order = CFG_LCD_RGB_ORDER;
      _panel.config(cfg);
    }
    {
      auto cfg = _light.config();          // 背光 PWM
      cfg.pin_bl = CFG_LCD_BL;
      cfg.freq = 12800;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    setPanel(&_panel);
  }

 private:
  lgfx::Bus_SPI _bus;
  PanelChip _panel;
  lgfx::Light_PWM _light;
};

namespace {
DashboardPanel lcd;

// LVGL 部分刷新缓冲：320×24×2 字节(RGB565) 单缓冲 = 15KB，共两个 ≈ 30KB
// ESP32 DRAM 紧张（BLE 模式要链接 NimBLE，静态 RAM 逼近 dram0_0_seg 上限），
// 双缓冲只用 30KB；800ms 数据节拍下渲染带宽绰绰有余
constexpr int32_t kBufPixels = CFG_LCD_WIDTH * 24;
lv_color_t buf1[kBufPixels];
// 刷新层 90° 旋转用：脏区转置后像素数不变，最大 24×320，同尺寸即可
lv_color_t rotBuf[kBufPixels];

// LVGL 9 刷新回调：把脏区像素推给 LovyanGFX
//
// 面板初始化后驱动坐标是竖装 240×320，而 UI 画布是横屏 320×240。
// 这里把每块脏区转置后再推，屏幕上看就是横屏；CFG_LCD_FLUSH_ROTATION
// 取 90/270 决定顺时针还是逆时针（两者差 180°，用来纠正上下颠倒）。
void flushCb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
  const int32_t w = area->x2 - area->x1 + 1;
  const int32_t h = area->y2 - area->y1 + 1;
  auto* src = reinterpret_cast<const lgfx::rgb565_t*>(px_map);

#if CFG_LCD_FLUSH_ROTATION == 0
  lcd.pushImage(area->x1, area->y1, w, h, src);
#else
  auto* dst = reinterpret_cast<lgfx::rgb565_t*>(rotBuf);
  if (CFG_LCD_FLUSH_ROTATION == 90) {
    // (x,y) -> (drvW-1-y, x)，drvW = 240 = CFG_LCD_HEIGHT
    const int32_t dx = CFG_LCD_HEIGHT - 1 - area->y2;
    const int32_t dy = area->x1;
    for (int32_t j = 0; j < h; ++j) {
      for (int32_t i = 0; i < w; ++i) {
        dst[i * h + (h - 1 - j)] = src[j * w + i];
      }
    }
    lcd.pushImage(dx, dy, h, w, dst);
  } else {  // 270: (x,y) -> (y, 319-x)，319 = CFG_LCD_WIDTH-1
    const int32_t dx = area->y1;
    const int32_t dy = CFG_LCD_WIDTH - 1 - area->x2;
    for (int32_t j = 0; j < h; ++j) {
      for (int32_t i = 0; i < w; ++i) {
        dst[(w - 1 - i) * h + j] = src[j * w + i];
      }
    }
    lcd.pushImage(dx, dy, h, w, dst);
  }
#endif
  lv_display_flush_ready(disp);
}
}  // namespace

// lv_tick 用 Arduino millis()，避免再开一个定时器
static uint32_t tickCb() { return millis(); }

void Display::begin() {
  // TF 卡(IO5)和触摸(IO33)与 LCD 共用 SPI 总线：
  // 片选浮空会抢总线导致 LCD 写入失败，先显式拉高禁用
  pinMode(5, OUTPUT);    // TF 卡 CS
  digitalWrite(5, HIGH);
  pinMode(33, OUTPUT);   // XPT2046 触摸 CS
  digitalWrite(33, HIGH);

  lcd.init();
  lcd.setRotation(CFG_LCD_ROTATION);  // 默认 1 = 横屏 320×240
  if (CFG_LCD_BL >= 0) {
    lcd.setBrightness(255);
  }
  lcd.fillScreen(TFT_BLACK);

  lv_init();
  lv_tick_set_cb(tickCb);

  // LVGL 对象池已在 lv_init() 里由 lvglPoolAlloc() 从堆整块申请（LV_MEM_SIZE=64KB）。
  // 之前用 lv_mem_add_pool() 追加池失败的原因：TLSF 的单池上限由 LV_MEM_SIZE 决定
  // （block_size_max = 1 << log2ceil(LV_MEM_SIZE)），静态池只有 8KB 时追加 56KB
  // 会被 lv_tlsf_add_pool 直接拒绝，返回 0 且只打一行 LV_LOG（LV_USE_LOG=0 时看不到）。
  lv_mem_monitor_t mon0;
  lv_mem_monitor(&mon0);
  CFG_LOG("LVGL 堆: 共 %uKB, ESP 空闲 %u B / 最大块 %u B",
          (unsigned)(mon0.total_size / 1024), (unsigned)ESP.getFreeHeap(),
          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

  lv_display_t* disp =
      lv_display_create(CFG_LCD_WIDTH, CFG_LCD_HEIGHT);
  lv_display_set_flush_cb(disp, flushCb);
  lv_display_set_buffers(disp, buf1, nullptr, sizeof(buf1),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);

  m_ok = true;
  CFG_LOG("屏幕就绪: %s 驱动%dx%d 刷新旋转%d SPI %luMHz  背光 GPIO%d",
          CFG_LCD_DRIVER_ILI9341 ? "ILI9341" : "ST7789", (int)lcd.width(),
          (int)lcd.height(), CFG_LCD_FLUSH_ROTATION,
          (unsigned long)(CFG_LCD_SPI_FREQ / 1000000UL), CFG_LCD_BL);
  // 刷新层旋转依赖「驱动坐标 = 竖装 240×320」这个前提
  if ((int)lcd.width() != CFG_LCD_HEIGHT || (int)lcd.height() != CFG_LCD_WIDTH) {
    CFG_LOG("警告: 驱动尺寸 %dx%d 与面板配置不符，旋转后画面会错位",
            (int)lcd.width(), (int)lcd.height());
  }
}

void Display::loop() {
  if (!m_ok) return;
  lv_timer_handler();

  // LVGL 堆水位监控：lv_malloc 返回 NULL 时 LVGL 不检查，会直接写 NULL+offset
  // 触发 StoreProhibited（EXCVADDR 0x8），很难从 backtrace 看出来。
  // 启动后 10s 打一次，之后每 30s 一次；用到 85% 以上立刻告警。
  static uint32_t nextMs = 10000;
  static bool first = true;
  if ((int32_t)(millis() - nextMs) >= 0) {
    nextMs = millis() + 30000;
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    if (first || mon.used_pct >= 85) {
      first = false;
      CFG_LOG("%s LVGL 堆: %u%% 已用 (%u/%u B), 峰值 %u B, 碎片 %u%% | ESP 空闲 %u B, 最低 %u B",
              mon.used_pct >= 85 ? "警告:" : "监控:",
              (unsigned)mon.used_pct,
              (unsigned)(mon.total_size - mon.free_size),
              (unsigned)mon.total_size, (unsigned)mon.max_used,
              (unsigned)mon.frag_pct, (unsigned)ESP.getFreeHeap(),
              (unsigned)ESP.getMinFreeHeap());
    }
  }
}
