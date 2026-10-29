#include "display/Display.h"

#include <LovyanGFX.hpp>
#include <lvgl.h>

#include "config.h"

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
      cfg.invert = true;                   // 颜色反了改这里
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
}
