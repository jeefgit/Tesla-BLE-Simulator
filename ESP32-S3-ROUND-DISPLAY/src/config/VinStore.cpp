#include "config/VinStore.h"

#include <Preferences.h>
#include <ctype.h>
#include <string.h>

#include "config.h"
#include "log/EventLog.h"

namespace {

constexpr char kPrefsNamespace[] = "tesla";
constexpr char kPrefsVinKey[] = "vin";

char s_vin[vinstore::kCapacity] = {};
bool s_isDefault = true;

bool isVinChar(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

bool isStrictForbidden(char c) {
  return c == 'I' || c == 'O' || c == 'Q';
}

// 只用于日志：返回最后 6 个字符；不足 6 位时返回原串。禁止打印完整 VIN。
const char* tail6(const char* vin) {
  if (vin == nullptr) {
    return "";
  }
  const size_t n = strlen(vin);
  return n > 6 ? vin + (n - 6) : vin;
}

void setCurrent(const char* vin, bool isDefault) {
  strncpy(s_vin, vin != nullptr ? vin : "", vinstore::kLength);
  s_vin[vinstore::kLength] = '\0';
  s_isDefault = isDefault;
}

bool readNvs(char out[vinstore::kCapacity]) {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, true)) {
    return false;
  }
  const size_t n = prefs.getString(kPrefsVinKey, out, vinstore::kCapacity);
  prefs.end();
  // Preferences::getString() returns the stored size including the trailing
  // '\0' (17 chars -> 18). Only n == 0 means the key is missing/unreadable.
  if (n == 0) {
    out[0] = '\0';
    return false;
  }
  out[vinstore::kLength] = '\0';
  return true;
}

}  // namespace

namespace vinstore {

void begin() {
  char stored[kCapacity] = {};
  if (readNvs(stored) && validate(stored) == Error::None) {
    setCurrent(stored, false);
    CFG_LOG("[VIN] source=nvs tail=%s", tail6(s_vin));
    EVT_LOGI("VIN", "source=nvs tail=%s", tail6(s_vin));
    return;
  }

  if (validate(CFG_TESLA_VIN) == Error::None) {
    setCurrent(CFG_TESLA_VIN, true);
    CFG_LOG("[VIN] source=default tail=%s", tail6(s_vin));
    EVT_LOGI("VIN", "source=default tail=%s", tail6(s_vin));
    return;
  }

  // NVS 值与出厂默认都非法：保留 NVS 方便排查，只回退为空。
  s_vin[0] = '\0';
  s_isDefault = true;
  CFG_LOG("[VIN] WARN no valid VIN; BLE falls back to prefix/UUID scan");
  EVT_LOGW("VIN", "no valid VIN; fallback scan");
}

const char* current() { return s_vin; }

bool isDefault() { return s_isDefault; }

Error validate(const char* vin) {
  if (vin == nullptr || vin[0] == '\0') {
    return Error::Empty;
  }
  const size_t n = strlen(vin);
  if (n != kLength) {
    return Error::Length;
  }
  for (size_t i = 0; i < n; ++i) {
    if (!isVinChar(vin[i])) {
      return Error::InvalidChar;
    }
#if CFG_VIN_STRICT_ISO
    if (isStrictForbidden(vin[i])) {
      return Error::StrictIso;
    }
#endif
  }
  return Error::None;
}

Error normalize(const char* in, char out[kCapacity]) {
  if (out == nullptr) {
    return Error::Empty;
  }
  out[0] = '\0';
  if (in == nullptr || in[0] == '\0') {
    return Error::Empty;
  }
  const size_t n = strlen(in);
  if (n != kLength) {
    return Error::Length;
  }
  for (size_t i = 0; i < n; ++i) {
    const char c = static_cast<char>(toupper(static_cast<unsigned char>(in[i])));
    if (!isVinChar(c)) {
      out[0] = '\0';
      return Error::InvalidChar;
    }
    out[i] = c;
  }
  out[n] = '\0';
#if CFG_VIN_STRICT_ISO
  for (size_t i = 0; i < n; ++i) {
    if (isStrictForbidden(out[i])) {
      out[0] = '\0';
      return Error::StrictIso;
    }
  }
#endif
  return Error::None;
}

Error save(const char* vin) {
  char normalized[kCapacity] = {};
  const Error err = normalize(vin, normalized);
  if (err != Error::None) {
    return err;
  }

  // 与出厂默认完全相同就不必占用 NVS。
  if (strcmp(normalized, CFG_TESLA_VIN) == 0) {
    if (!clear()) {
      return Error::NvsWrite;
    }
    CFG_LOG("[VIN] matches factory default; nvs override cleared");
    return Error::None;
  }

  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false)) {
    EVT_LOGE("VIN", "save failed: nvs open");
    return Error::NvsWrite;
  }
  const size_t written = prefs.putString(kPrefsVinKey, normalized);
  prefs.end();
  if (written != kLength) {
    EVT_LOGE("VIN", "save failed: putString=%u",
             static_cast<unsigned>(written));
    return Error::NvsWrite;
  }

  // 读回校验，避免写入截断或旧值残留。
  char readback[kCapacity] = {};
  Preferences check;
  if (!check.begin(kPrefsNamespace, true)) {
    EVT_LOGE("VIN", "save failed: nvs reopen");
    return Error::NvsWrite;
  }
  const size_t n = check.getString(kPrefsVinKey, readback, kCapacity);
  check.end();
  if (n == 0 || strcmp(readback, normalized) != 0) {
    EVT_LOGE("VIN", "save failed: verify n=%u", static_cast<unsigned>(n));
    return Error::NvsWrite;
  }

  setCurrent(normalized, false);
  CFG_LOG("[VIN] saved tail=%s", tail6(s_vin));
  EVT_LOGI("VIN", "saved tail=%s", tail6(s_vin));
  return Error::None;
}

bool clear() {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false)) {
    EVT_LOGE("VIN", "clear failed: nvs open");
    return false;
  }
  // erase_key() reports ESP_ERR_NVS_NOT_FOUND when the key never existed
  // (e.g. the user re-enters the factory default VIN). Treat that as success.
  const bool existed = prefs.isKey(kPrefsVinKey);
  const bool removed = existed ? prefs.remove(kPrefsVinKey) : true;
  prefs.end();
  if (!removed) {
    EVT_LOGE("VIN", "clear failed: remove");
    return false;
  }

  if (validate(CFG_TESLA_VIN) == Error::None) {
    setCurrent(CFG_TESLA_VIN, true);
    CFG_LOG("[VIN] cleared; source=default tail=%s", tail6(s_vin));
    EVT_LOGI("VIN", "cleared; source=default tail=%s", tail6(s_vin));
  } else {
    s_vin[0] = '\0';
    s_isDefault = true;
    CFG_LOG("[VIN] cleared; no valid default VIN");
    EVT_LOGW("VIN", "cleared; no valid default VIN");
  }
  return true;
}

const char* errorText(Error error) {
  switch (error) {
    case Error::None:
      return "";
    case Error::Empty:
      return "VIN IS EMPTY";
    case Error::Length:
      return "VIN MUST BE 17 CHARACTERS";
    case Error::InvalidChar:
      return "USE A-Z AND 0-9 ONLY";
    case Error::StrictIso:
      return "VIN CANNOT CONTAIN I/O/Q";
    case Error::NvsWrite:
      return "SAVE FAILED";
  }
  return "VIN ERROR";
}

}  // namespace vinstore
