#include "ota/UsbOta.h"

#include <Arduino.h>
#include <Update.h>
#include <esp_image_format.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_rom_crc.h>

#include <cstdio>
#include <cstring>

#include "display/IDisplay.h"

#if CFG_USB_OTA_ENABLE

namespace {

const char* partLabel(const esp_partition_t* p) { return p != nullptr ? p->label : "?"; }

const char* stateName(unsigned int s) {
  switch (s) {
    case ESP_OTA_IMG_NEW:
      return "NEW";
    case ESP_OTA_IMG_PENDING_VERIFY:
      return "PENDING_VERIFY";
    case ESP_OTA_IMG_VALID:
      return "VALID";
    case ESP_OTA_IMG_INVALID:
      return "INVALID";
    case ESP_OTA_IMG_ABORTED:
      return "ABORTED";
    case ESP_OTA_IMG_UNDEFINED:
      return "UNDEFINED";
    default:
      return "?";
  }
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

  // 注意：不要用 esp_app_desc 里的 version/date 判断“跑的是哪一版”。
  // 这个 PlatformIO 配置下镜像里的 app desc 来自预编译的 core 库
  // （project=arduino-lib-builder，日期是 core 库的构建时间），
  // 所有构建都一模一样。用 config.h 里的编译时间戳才靠谱。
  CFG_LOG("[OTA] fw %s built %s", CFG_FW_VERSION, CFG_FW_BUILD);
  CFG_LOG("[OTA] running=%s boot=%s", partLabel(esp_ota_get_running_partition()),
          partLabel(esp_ota_get_boot_partition()));
  // 如果升级后这里出现 ABORTED，说明新镜像启动后没来得及调用
  // esp_ota_mark_app_valid_cancel_rollback() 就被复位了（被回滚）。
  dumpOtadata();
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

void UsbOta::dumpOtadata() {
  // otadata 两个 sector 各存一条记录。bootloader 的规则：
  //   state 为 INVALID/ABORTED、或 ota_seq=0xFFFFFFFF、或 CRC 不对 → 该条无效；
  //   PENDING_VERIFY 在下一次启动时会被 bootloader 直接改成 ABORTED。
  // 两条都无效时 bootloader 只能回退到 app0，这就是“升级成功却还在 app0”的成因。
  const esp_partition_t* p = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
  if (p == nullptr) {
    Serial.println("OTA: OTADATA none");
    return;
  }
  const int slotCount = static_cast<int>(esp_ota_get_app_partition_count());
  for (int i = 0; i < 2; ++i) {
    esp_ota_select_entry_t e = {};
    if (esp_partition_read(p, static_cast<size_t>(i) * p->erase_size, &e,
                           sizeof(e)) != ESP_OK) {
      Serial.printf("OTA: OTADATA[%d] readfail\n", i);
      continue;
    }
    const uint32_t crc = esp_rom_crc32_le(
        UINT32_MAX, reinterpret_cast<uint8_t*>(&e.ota_seq), 4);
    const bool seqSet = e.ota_seq != UINT32_MAX;
    const bool crcOk = e.crc == crc;
    const bool usable = seqSet && crcOk &&
                        e.ota_state != ESP_OTA_IMG_INVALID &&
                        e.ota_state != ESP_OTA_IMG_ABORTED;
    if (!seqSet || !crcOk || slotCount <= 0) {
      Serial.printf("OTA: OTADATA[%d] empty (seq=%lu crc=%s)\n", i,
                    static_cast<unsigned long>(e.ota_seq), crcOk ? "ok" : "bad");
      continue;
    }
    const int slot =
        static_cast<int>((e.ota_seq - 1) % static_cast<uint32_t>(slotCount));
    Serial.printf("OTA: OTADATA[%d] seq=%lu slot=app%d state=%d(%s) crc=%s %s\n",
                  i, static_cast<unsigned long>(e.ota_seq), slot,
                  static_cast<int>(e.ota_state),
                  stateName(static_cast<int>(e.ota_state)), crcOk ? "ok" : "bad",
                  usable ? "usable" : "NOT-usable");
  }
}

void UsbOta::dumpInfo() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* boot = esp_ota_get_boot_partition();

  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  int stateValue = -1;
  if (boot != nullptr && esp_ota_get_state_partition(boot, &state) == ESP_OK) {
    stateValue = static_cast<int>(state);
  }
  Serial.printf("OTA: INFO run=%s boot=%s state=%d(%s)\n", partLabel(running),
                partLabel(boot), stateValue, stateName(stateValue));

  // 两个 OTA 分区里是不是同一份固件，没法从镜像里读出来：
  // 这个 PlatformIO 配置下镜像中的 app desc 来自预编译的 core 库
  // （project=arduino-lib-builder），每次编译都相同，拿它对比只会得出
  // “两槽一样”的错误结论。所以这里只报“槽里有没有有效镜像”，不再报 version/date。
  struct Slot {
    const char* label;
    esp_partition_subtype_t subtype;
  };
  const Slot slots[] = {
      {"app0", ESP_PARTITION_SUBTYPE_APP_OTA_0},
      {"app1", ESP_PARTITION_SUBTYPE_APP_OTA_1},
  };
  for (const Slot& slot : slots) {
    const esp_partition_t* p =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, slot.subtype, nullptr);
    if (p == nullptr) {
      Serial.printf("OTA: IMG %s=none\n", slot.label);
      continue;
    }
    // 每个槽自己的 otadata 状态（不是“当前启动分区”的状态）。
    esp_ota_img_states_t slotState = ESP_OTA_IMG_UNDEFINED;
    const int slotStateValue =
        esp_ota_get_state_partition(p, &slotState) == ESP_OK
            ? static_cast<int>(slotState)
            : -1;
    uint8_t magic = 0;
    const char* image = "empty";
    if (esp_partition_read(p, 0, &magic, sizeof(magic)) != ESP_OK) {
      image = "readfail";
    } else if (magic == ESP_IMAGE_HEADER_MAGIC) {
      image = "written";
    }
    Serial.printf("OTA: IMG %s=%s state=%d(%s)\n", slot.label, image,
                  slotStateValue, stateName(slotStateValue));
  }
  dumpOtadata();
  Serial.println("OTA: INFO done");
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
      // 第一块就是镜像头。这里不再读 app desc 里的 version/date：那个字段
      // 来自预编译的 core 库（arduino-lib-builder），每次编译都一样，拿它
      // 判断“是不是同一版固件”只会误报。镜像头 magic 能确认文件类型即可。
      Serial.printf("OTA: INFO incoming magic=%02x size=%lu\n",
                    static_cast<unsigned>(want > 0 ? s_chunk[0] : 0),
                    static_cast<unsigned long>(size));
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

    // 落盘的镜像内容已经用 CRC 回读核对过（上面 readback=ok），这里只报一下
    // 大小和分区，不再读 app desc（那是 core 库的，不是本工程的）。
    if (boot != nullptr) {
      Serial.printf("OTA: INFO written size=%lu part=%s\n",
                    static_cast<unsigned long>(size), partLabel(boot));
    }

    // 本 core 的 sdkconfig 打开了 CONFIG_APP_ROLLBACK_ENABLE：新镜像首次启动
    // 处于 PENDING_VERIFY，Arduino 的 initArduino() 会立刻标记为有效。这里把
    // 状态打出来，方便确认没有走回滚路径。
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (boot != nullptr && esp_ota_get_state_partition(boot, &state) == ESP_OK) {
      Serial.printf("OTA: INFO boot_state=%d(%s)\n", static_cast<int>(state),
                    stateName(static_cast<int>(state)));
    }
    dumpOtadata();

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
  lv_obj_set_scrollable(m_overlay, false);

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
