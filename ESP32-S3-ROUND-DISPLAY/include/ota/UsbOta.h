#pragma once

#include <stdint.h>

#include <lvgl.h>

#include "config.h"

class IDisplay;

// 通过 USB CDC（板载 USB 口）接收主机发来的 firmware.bin，写入另一个
// OTA app 分区，校验通过后自动重启。实现见 src/ota/UsbOta.cpp，
// 主机端工具见 tools/ota/index.html。
class UsbOta {
 public:
  // 在 display.begin() 之后调用。
  void begin(IDisplay* display);

  // 主循环调用。返回 true 表示收到了主机命令，调用方可借此重置自动休眠计时。
  // 收到 OTA_BEGIN 后本函数会阻塞到传输结束（成功则直接重启）。
  bool loop();

  bool transferring() const { return m_transferring; }

 private:
  void handleLine(const char* line);
  void dumpInfo();
  void startTransfer(uint32_t size, uint32_t crc);
  bool readChunk(uint8_t* dst, uint32_t len, uint32_t timeoutMs);

  void showOverlay();
  void setOverlay(const char* title, const char* percent, const char* hint);
  void hideOverlay();

  IDisplay* m_display = nullptr;
  char m_line[96] = {};
  uint8_t m_lineLen = 0;
  bool m_transferring = false;

  lv_obj_t* m_overlay = nullptr;
  lv_obj_t* m_titleLabel = nullptr;
  lv_obj_t* m_percentLabel = nullptr;
  lv_obj_t* m_hintLabel = nullptr;
};
