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
  CFG_LOG_ALWAYS("错误: LVGL 断言失败 (%s:%d) —— 内存不足或参数非法，重启",
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
    CFG_LOG_ALWAYS("错误: LVGL 池申请 %u B 失败（ESP 空闲 %u B / 最大块 %u B）",
                   (unsigned)size, (unsigned)ESP.getFreeHeap(),
                   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    Serial.flush();
    delay(100);
    esp_restart();  // 没有池，LVGL 后续必然写 NULL：直接重启等下次
  }
  return p;
}

// ---------------------------------------------------------------------------
// LovyanGFX 面板：所有引脚/尺寸/旋转来自 BoardProfile。
// 驱动芯片类型是编译期选择的（ILI9341 / ST7789），其余参数都是运行时。
//
// ⚠️ CYD 板屏幕调试点（换屏时先看这里）：
//   1. LCD 走 HSPI 14/13/12，18/23 是 TF 卡脚（接错则全白）
//   2. RST 与 MISO 共用 IO12
//   3. 克隆 ILI9341 按 320x240 声明才稳定（按 240x320 原生声明会花屏）
//   4. 屏幕水平镜像，用 offset_rotation=4 官方翻转位修正
//   5. TF 卡(IO5)/触摸(IO33) 与 LCD 共用 SPI 总线，启动先拉高片选
// ---------------------------------------------------------------------------
#if CFG_LCD_DRIVER_ILI9341
using PanelChip = lgfx::Panel_ILI9341;
#else
using PanelChip = lgfx::Panel_ST7789;
#endif

class DashboardPanel : public lgfx::LGFX_Device {
 public:
  void configure(const BoardProfile& b) {
    {
      auto cfg = _bus.config();  // SPI 总线
      cfg.pin_sclk = b.pinSck;
      cfg.pin_mosi = b.pinMosi;
      cfg.pin_miso = b.pinMiso;
      cfg.pin_dc = b.pinDc;
      cfg.spi_mode = 0;
      cfg.spi_host = SPI2_HOST;  // 显式 HSPI
      cfg.freq_write = b.spiHz;
      cfg.freq_read = 16000000;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();  // 面板
      cfg.pin_cs = b.pinCs;
      cfg.pin_rst = b.pinRst;
      cfg.panel_width = b.panelWidth;
      cfg.panel_height = b.panelHeight;
      cfg.memory_width = b.panelWidth;
      cfg.memory_height = b.panelHeight;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      // 水平镜像修正：rot1 + offset4 -> MADCTL 0xA8
      cfg.offset_rotation = b.offsetRotation;
      cfg.invert = b.invert;      // 1 = 反色
      cfg.rgb_order = b.rgbOrder; // true = RGB，false = BGR
      _panel.config(cfg);
    }
    {
      auto cfg = _light.config();  // 背光 PWM
      cfg.pin_bl = b.pinBl;
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

// 板型在 begin() 时写入，flush 回调里读取（LVGL 回调没有 user_data）
const BoardProfile* s_board = nullptr;

// LVGL 部分刷新缓冲上限：320×24×2 字节(RGB565) = 15KB。
// 实际行数按 BoardProfile::bufferLines 与屏宽裁剪，保证不越界。
constexpr int32_t kMaxBufPixels = 320 * 24;
lv_color_t buf1[kMaxBufPixels];
// 刷新层 90° 旋转用：脏区转置后像素数不变，最大 24×320，同尺寸即可
lv_color_t rotBuf[kMaxBufPixels];

// LVGL 9 刷新回调：把脏区像素推给 LovyanGFX
void flushCb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
  const int32_t w = area->x2 - area->x1 + 1;
  const int32_t h = area->y2 - area->y1 + 1;
  auto* src = reinterpret_cast<const lgfx::rgb565_t*>(px_map);
  const int rotation = s_board != nullptr ? s_board->flushRotation : 0;

  if (rotation == 0 || s_board == nullptr) {
    lcd.pushImage(area->x1, area->y1, w, h, src);
  } else {
    auto* dst = reinterpret_cast<lgfx::rgb565_t*>(rotBuf);
    if (rotation == 90) {
      // (x,y) -> (drvW-1-y, x)，drvW = 屏高
      const int32_t dx = s_board->height - 1 - area->y2;
      const int32_t dy = area->x1;
      for (int32_t j = 0; j < h; ++j) {
        for (int32_t i = 0; i < w; ++i) {
          dst[i * h + (h - 1 - j)] = src[j * w + i];
        }
      }
      lcd.pushImage(dx, dy, h, w, dst);
    } else {  // 270: (x,y) -> (y, 宽-1-x)
      const int32_t dx = area->y1;
      const int32_t dy = s_board->width - 1 - area->x2;
      for (int32_t j = 0; j < h; ++j) {
        for (int32_t i = 0; i < w; ++i) {
          dst[(w - 1 - i) * h + j] = src[j * w + i];
        }
      }
      lcd.pushImage(dx, dy, h, w, dst);
    }
  }
  lv_display_flush_ready(disp);
}
}  // namespace

// lv_tick 用 Arduino millis()，避免再开一个定时器
static uint32_t tickCb() { return millis(); }

bool Display::begin(const BoardProfile& b) {
  s_board = &b;

  // 与 LCD 共用 SPI 总线的外设片选先拉高，避免抢总线导致白屏
  if (b.pinTfCs >= 0) {
    pinMode(b.pinTfCs, OUTPUT);
    digitalWrite(b.pinTfCs, HIGH);
  }
  if (b.pinTouchCs >= 0) {
    pinMode(b.pinTouchCs, OUTPUT);
    digitalWrite(b.pinTouchCs, HIGH);
  }

  lcd.configure(b);
  lcd.init();
  lcd.setRotation(b.rotation);
  if (b.pinBl >= 0) {
    lcd.setBrightness(255);
  }
  lcd.fillScreen(TFT_BLACK);

  lv_init();
  lv_tick_set_cb(tickCb);

  lv_mem_monitor_t mon0;
  lv_mem_monitor(&mon0);
  CFG_LOG("LVGL 堆: 共 %uKB, ESP 空闲 %u B / 最大块 %u B",
          (unsigned)(mon0.total_size / 1024), (unsigned)ESP.getFreeHeap(),
          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

  lv_display_t* disp = lv_display_create(b.width, b.height);
  lv_display_set_flush_cb(disp, flushCb);

  // 按板型裁剪缓冲行数，保证不超过静态缓冲区
  int32_t lines = b.bufferLines;
  if (lines < 4) lines = 4;
  if (lines * b.width > kMaxBufPixels) lines = kMaxBufPixels / b.width;
  if (lines < 4) lines = 4;
  lv_display_set_buffers(disp, buf1, nullptr,
                         (uint32_t)(lines * b.width * sizeof(lv_color_t)),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);

  m_ok = true;
  CFG_LOG("屏幕就绪: %s 驱动%dx%d 刷新旋转%d SPI %luMHz  背光 GPIO%d",
          b.name, (int)lcd.width(), (int)lcd.height(), b.flushRotation,
          (unsigned long)(b.spiHz / 1000000UL), b.pinBl);
  if ((int)lcd.width() != b.height || (int)lcd.height() != b.width) {
    CFG_LOG("警告: 驱动尺寸 %dx%d 与面板配置不符，旋转后画面会错位",
            (int)lcd.width(), (int)lcd.height());
  }
  return m_ok;
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
