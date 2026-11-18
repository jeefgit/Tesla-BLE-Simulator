#include "ota/UsbOta.h"

#include <Arduino.h>
#include <Update.h>
#include <esp_app_desc.h>
#include <esp_image_format.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

#include <cstdio>
#include <cstring>

#include "display/IDisplay.h"

#if CFG_USB_OTA_ENABLE

namespace {

const char* partLabel(const esp_partition_t* p) { return p != nullptr ? p->label : "?"; }

// 从任意 app 分区读出镜像头里的 esp_app_desc（版本 / 编译日期时间）。
// app 镜像 = image_header(24B) + 第一个 segment header(8B) + app desc(256B)，
// 所以 desc 固定在偏移 32 处。失败时返回 false。
bool readAppDesc(const esp_partition_t* p, esp_app_desc_t* out) {
  if (p == nullptr || out == nullptr) {
    return false;
  }
  const size_t offset = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
  if (esp_partition_read(p, offset, out, sizeof(*out)) != ESP_OK) {
    return false;
  }
  return out->magic_word == ESP_APP_DESC_MAGIC_WORD;
}

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

void UsbOta::begin(IDisplay* display) {
  m_display = display;

  // 启动时把“镜像自带的编译信息”和当前运行/启动分区打到串口：
  //   running= 现在跑的是哪个 app 分区
  //   boot=    otadata 里记录的下次启动分区
  // 正常时两者相同；OTA 完成后 boot 会变成另一个分区。
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* boot = esp_ota_get_boot_partition();

  esp_app_desc_t desc = {};
  if (readAppDesc(running, &desc)) {
    CFG_LOG("[OTA] fw %s built %s %s", desc.version, desc.date, desc.time);
  } else {
    const esp_app_desc_t* app = esp_app_get_description();
    CFG_LOG("[OTA] fw %s built %s %s", app != nullptr ? app->version : "?",
            app != nullptr ? app->date : "?", app != nullptr ? app->time : "?");
  }
  CFG_LOG("[OTA] running=%s boot=%s", partLabel(running), partLabel(boot));
}

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
    // 带上编译时间戳和当前运行分区，网页握手时就能确认到底跑的是哪一版。
    Serial.printf("OTA: PONG ver=%s build=%s part=%s\n", CFG_FW_VERSION,
                  CFG_FW_BUILD, partLabel(esp_ota_get_running_partition()));
    return;
  }
  if (strcmp(line, "OTA_INFO") == 0) {
    dumpInfo();
    return;
  }
  if (strcmp(line, "OTA_REBOOT") == 0) {
    // 单独验证“重启”这条路径是否可用：用来区分“OTA 没写进去”和
    // “写进去了但重启后没生效”。
    Serial.println("OTA: INFO rebooting");
    Serial.flush();
    delay(200);
    esp_restart();
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

void UsbOta::dumpInfo() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* boot = esp_ota_get_boot_partition();

  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  int stateValue = -1;
  if (boot != nullptr && esp_ota_get_state_partition(boot, &state) == ESP_OK) {
    stateValue = static_cast<int>(state);
  }
  Serial.printf("OTA: INFO run=%s boot=%s state=%d\n", partLabel(running),
                partLabel(boot), stateValue);

  // 两个 OTA 分区里的镜像各自带什么编译时间，是判断“到底哪一版在跑”的铁证。
  struct Slot {
    const char* label;
    esp_partition_subtype_t subtype;
  };
  const Slot slots[] = {
      {"app0", ESP_PARTITION_SUBTYPE_APP_OTA_0},
      {"app1", ESP_PARTITION_SUBTYPE_APP_OTA_1},
  };
  for (const Slot& slot : slots) {
    const esp_partition_t* p = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, slot.subtype, nullptr);
    if (p == nullptr) {
      Serial.printf("OTA: IMG %s=none\n", slot.label);
      continue;
    }
    esp_app_desc_t desc = {};
    if (readAppDesc(p, &desc)) {
      Serial.printf("OTA: IMG %s=%s@%s %s\n", slot.label, desc.version,
                    desc.date, desc.time);
    } else {
      Serial.printf("OTA: IMG %s=empty\n", slot.label);
    }
  }
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

    if (received == 0) {
      // 第一块里就包含镜像头 + app desc（版本 / 编译日期时间）。直接读出来，
      // 立刻就能看出“你传的到底是哪一版”，不用等升级完。
      const size_t descOffset =
          sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
      esp_app_desc_t incoming = {};
      if (want >= descOffset + sizeof(incoming)) {
        memcpy(&incoming, s_chunk + descOffset, sizeof(incoming));
      }
      if (incoming.magic_word == ESP_APP_DESC_MAGIC_WORD) {
        Serial.printf("OTA: INFO incoming=%s built %s %s\n", incoming.version,
                      incoming.date, incoming.time);
      } else {
        Serial.println("OTA: INFO incoming=unknown");
      }

      esp_app_desc_t current = {};
      if (readAppDesc(esp_ota_get_running_partition(), &current)) {
        Serial.printf("OTA: INFO current=%s built %s %s\n", current.version,
                      current.date, current.time);
        if (incoming.magic_word == ESP_APP_DESC_MAGIC_WORD &&
            strcmp(incoming.date, current.date) == 0 &&
            strcmp(incoming.time, current.time) == 0) {
          Serial.println("OTA: INFO same_image");
        }
      }
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

  // Update.end() 成功后 esp_ota_set_boot_partition() 应已把 otadata 指向
  // 另一个 app 分区。这里读回来核对一次：如果 boot 仍等于 running，说明
  // 分区根本没切换（例如分区表没有第二个 OTA 分区），直接报错而不是假装成功。
  if (ok) {
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* boot = esp_ota_get_boot_partition();

    // 回读校验：Update.end() 里的 _partitionIsBootable() 只看首字节 magic，
    // 写坏的镜像它发现不了。这里把真正落盘的 size 字节读回来重算一次 CRC。
    if (boot != nullptr) {
      setOverlay("USB OTA", "100%", "Verifying...");
      uint32_t readbackCrc = 0xFFFFFFFFu;
      uint32_t offset = 0;
      bool readOk = true;
      while (offset < size) {
        uint32_t want = size - offset;
        if (want > CFG_USB_OTA_CHUNK) {
          want = CFG_USB_OTA_CHUNK;
        }
        if (esp_partition_read(boot, offset, s_chunk, want) != ESP_OK) {
          readOk = false;
          break;
        }
        readbackCrc = crc32Update(readbackCrc, s_chunk, want);
        offset += want;
      }
      readbackCrc ^= 0xFFFFFFFFu;

      if (!readOk || readbackCrc != crc) {
        Serial.printf("OTA: ERR readback (got %08lx want %08lx)\n",
                      static_cast<unsigned long>(readbackCrc),
                      static_cast<unsigned long>(crc));
        // flash 内容和主机文件不一致：把启动分区改回去，别让设备去跑坏镜像。
        if (running != nullptr) {
          esp_ota_set_boot_partition(running);
        }
        ok = false;
      } else {
        Serial.println("OTA: INFO readback=ok");
      }
    }
  }

  if (ok) {
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* boot = esp_ota_get_boot_partition();

    // 把刚写入的镜像自带的编译时间读出来：这是“到底传进去了哪个文件”的铁证。
    esp_app_desc_t written = {};
    if (boot != nullptr && readAppDesc(boot, &written)) {
      Serial.printf("OTA: INFO written=%s built %s %s part=%s\n",
                    written.version, written.date, written.time,
                    partLabel(boot));
    }

    // 本 core 的 sdkconfig 打开了 CONFIG_APP_ROLLBACK_ENABLE：新镜像首次启动
    // 处于 PENDING_VERIFY，Arduino 的 initArduino() 会立刻标记为有效。这里把
    // 状态打出来，方便确认没有走回滚路径。
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (boot != nullptr && esp_ota_get_state_partition(boot, &state) == ESP_OK) {
      Serial.printf("OTA: INFO boot_state=%d\n", static_cast<int>(state));
    }

    if (running != nullptr && boot != nullptr && boot->address == running->address) {
      Serial.printf("OTA: ERR boot partition not switched (still %s)\n",
                    boot->label);
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

  Serial.printf("OTA: OK boot=%s\n",
                partLabel(esp_ota_get_boot_partition()));
  Serial.flush();
  setOverlay("USB OTA", "100%", "Rebooting...");
  delay(1200);
  ESP.restart();
  // ESP.restart() 正常情况下不会返回；万一被中断，兜底再触发一次硬复位。
  delay(200);
  esp_restart();
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
