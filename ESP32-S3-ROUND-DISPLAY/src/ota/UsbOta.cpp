#include "ota/UsbOta.h"

#include <Arduino.h>
#include <Update.h>

#include <cstdio>
#include <cstring>

#include "display/IDisplay.h"

#if CFG_USB_OTA_ENABLE

namespace {

// 主机每发一块就等一次 ACK：flash 写入和界面刷新期间不会有新数据在途，
// 避免 USB CDC 默认 256B 接收缓冲在 flash 操作期间溢出丢字节。
uint8_t s_chunk[CFG_USB_OTA_CHUNK];

// 标准 CRC32（IEEE 802.3，反射，多项式 0xEDB88320），与网页端实现一致。
uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    uint32_t c = (crc ^ data[i]) & 0xFFu;
    for (int bit = 0; bit < 8; ++bit) {
      c = (c & 1u) ? ((c >> 1) ^ 0xEDB88320u) : (c >> 1);
    }
    crc = (crc >> 8) ^ c;
  }
  return crc;
}

}  // namespace

void UsbOta::begin(IDisplay* display) { m_display = display; }

bool UsbOta::loop() {
  // 传输期间本函数阻塞在 startTransfer() 内，不会走到这里。
  if (m_transferring) {
    return false;
  }
  bool activity = false;
  while (Serial.available() > 0) {
    const int value = Serial.read();
    if (value < 0) {
      break;
    }
    activity = true;
    if (value == '\r') {
      continue;
    }
    if (value == '\n') {
      if (m_lineLen > 0) {
        m_line[m_lineLen] = '\0';
        handleLine(m_line);
        m_lineLen = 0;
      }
      continue;
    }
    if (m_lineLen < sizeof(m_line) - 1) {
      m_line[m_lineLen++] = static_cast<char>(value);
    } else {
      // 超长行直接丢弃，等待下一个换行重新同步。
      m_lineLen = 0;
    }
  }
  return activity;
}

void UsbOta::handleLine(const char* line) {
  if (strcmp(line, "OTA_PING") == 0) {
    Serial.printf("OTA: PONG %s\n", CFG_FW_VERSION);
    return;
  }
  if (strncmp(line, "OTA_BEGIN ", 10) == 0) {
    unsigned long size = 0;
    unsigned long crc = 0;
    if (sscanf(line + 10, "%lu %lx", &size, &crc) != 2 || size == 0) {
      Serial.println("OTA: ERR args");
      return;
    }
    startTransfer(static_cast<uint32_t>(size), static_cast<uint32_t>(crc));
    return;
  }
  // 其他串口输入（例如串口监视器里敲的字符）一律忽略，不回显、不报错。
}

void UsbOta::startTransfer(uint32_t size, uint32_t crc) {
#if CFG_DEBUG_LOG
  CFG_LOG("[OTA] begin size=%lu crc=%08lx",
          static_cast<unsigned long>(size),
          static_cast<unsigned long>(crc));
#endif

  if (!Update.begin(size, U_FLASH)) {
    Serial.printf("OTA: ERR begin %s\n", Update.errorString());
    return;
  }

  m_transferring = true;
#if CFG_DEBUG_LOG
  // 升级期间屏蔽 BLE/LVGL 的常规日志，避免主机端行解析被打断。
  cfglog::setMuted(true);
#endif
  showOverlay();
  setOverlay("USB OTA", "0%", "Do not unplug");

  Serial.printf("OTA: READY %lu\n", static_cast<unsigned long>(size));
  Serial.flush();

  uint32_t received = 0;
  uint32_t runningCrc = 0xFFFFFFFFu;
  uint32_t lastUiMs = 0;
  bool ok = true;

  while (received < size) {
    uint32_t want = size - received;
    if (want > CFG_USB_OTA_CHUNK) {
      want = CFG_USB_OTA_CHUNK;
    }
    if (!readChunk(s_chunk, want, CFG_USB_OTA_RX_TIMEOUT_MS)) {
      Serial.println("OTA: ERR timeout");
      ok = false;
      break;
    }

    runningCrc = crc32Update(runningCrc, s_chunk, want);
    if (Update.write(s_chunk, want) != want) {
      Serial.printf("OTA: ERR write %s\n", Update.errorString());
      ok = false;
      break;
    }
    received += want;

    // 先刷新界面再发 ACK：ACK 发出后主机立刻发下一块，此时设备已经回到
    // readChunk()，不会让界面刷新占用接收窗口。
    const uint32_t now = millis();
    if (now - lastUiMs >= 120 || received == size) {
      lastUiMs = now;
      char percent[8];
      snprintf(percent, sizeof(percent), "%lu%%",
               static_cast<unsigned long>(
                   static_cast<uint64_t>(received) * 100u / size));
      setOverlay("USB OTA", percent, "Do not unplug");
    }
    Serial.printf("OTA: A %lu\n", static_cast<unsigned long>(received));
  }

  if (ok) {
    setOverlay("USB OTA", "100%", "Checking...");
    const uint32_t finalCrc = runningCrc ^ 0xFFFFFFFFu;
    if (finalCrc != crc) {
      Serial.printf("OTA: ERR crc (got %08lx want %08lx)\n",
                    static_cast<unsigned long>(finalCrc),
                    static_cast<unsigned long>(crc));
      ok = false;
    } else if (!Update.end()) {
      Serial.printf("OTA: ERR end %s\n", Update.errorString());
      ok = false;
    }
  }

  if (!ok) {
    if (Update.isRunning()) {
      Update.abort();
    }
    setOverlay("USB OTA", "--", "Update failed");
    Serial.flush();
    delay(2500);
    hideOverlay();
    // 丢弃主机残留数据，避免下一次连接时被当成命令解析。
    while (Serial.available() > 0) {
      Serial.read();
    }
#if CFG_DEBUG_LOG
    cfglog::setMuted(false);
#endif
    m_transferring = false;
    m_lineLen = 0;
    return;
  }

  Serial.println("OTA: OK");
  Serial.flush();
  setOverlay("USB OTA", "100%", "Rebooting...");
  delay(1200);
  ESP.restart();
}

bool UsbOta::readChunk(uint8_t* dst, uint32_t len, uint32_t timeoutMs) {
  uint32_t got = 0;
  uint32_t lastDataMs = millis();
  while (got < len) {
    const int n = static_cast<int>(Serial.read(dst + got, len - got));
    if (n > 0) {
      got += n;
      lastDataMs = millis();
      continue;
    }
    if (millis() - lastDataMs >= timeoutMs) {
      return false;
    }
    // 让出 CPU，避免 USB 事件/看门狗被饿死。
    delay(1);
  }
  return true;
}

void UsbOta::showOverlay() {
  if (m_overlay != nullptr) {
    return;
  }
  lv_obj_t* screen = lv_screen_active();

  m_overlay = lv_obj_create(screen);
  lv_obj_remove_style_all(m_overlay);
  lv_obj_set_size(m_overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(m_overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(m_overlay, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(m_overlay, 24, 0);
  lv_obj_set_style_pad_row(m_overlay, 14, 0);
  lv_obj_set_flex_flow(m_overlay, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(m_overlay, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_remove_flag(m_overlay, LV_OBJ_FLAG_SCROLLABLE);

  m_titleLabel = lv_label_create(m_overlay);
  lv_obj_set_style_text_font(m_titleLabel, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(m_titleLabel, lv_color_hex(0xE82127), 0);
  lv_label_set_text(m_titleLabel, "USB OTA");

  m_percentLabel = lv_label_create(m_overlay);
  lv_obj_set_style_text_font(m_percentLabel, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(m_percentLabel, lv_color_white(), 0);
  lv_label_set_text(m_percentLabel, "0%");

  m_hintLabel = lv_label_create(m_overlay);
  lv_obj_set_style_text_font(m_hintLabel, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(m_hintLabel, lv_color_hex(0x8A8A8E), 0);
  lv_label_set_text(m_hintLabel, "Do not unplug");

  if (m_display != nullptr) {
    m_display->loop();
  }
}

void UsbOta::setOverlay(const char* title, const char* percent,
                        const char* hint) {
  if (m_overlay == nullptr) {
    return;
  }
  if (m_titleLabel != nullptr && title != nullptr) {
    lv_label_set_text(m_titleLabel, title);
  }
  if (m_percentLabel != nullptr && percent != nullptr) {
    lv_label_set_text(m_percentLabel, percent);
  }
  if (m_hintLabel != nullptr && hint != nullptr) {
    lv_label_set_text(m_hintLabel, hint);
  }
  if (m_display != nullptr) {
    m_display->loop();
  }
}

void UsbOta::hideOverlay() {
  if (m_overlay == nullptr) {
    return;
  }
  lv_obj_delete(m_overlay);
  m_overlay = nullptr;
  m_titleLabel = nullptr;
  m_percentLabel = nullptr;
  m_hintLabel = nullptr;
  if (m_display != nullptr) {
    m_display->loop();
  }
}

#else  // !CFG_USB_OTA_ENABLE

void UsbOta::begin(IDisplay*) {}
bool UsbOta::loop() { return false; }

#endif
