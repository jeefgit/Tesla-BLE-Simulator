#include "config/ConfigMode.h"

#include <Preferences.h>

#include "config.h"
#include "log/EventLog.h"

namespace {

constexpr char kPrefsNamespace[] = "tesla";
constexpr char kPrefsKey[] = "cfg_mode";

bool s_active = false;

bool readFlag() {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, true)) {
    return false;
  }
  const bool on = prefs.getBool(kPrefsKey, false);
  prefs.end();
  return on;
}

void writeFlag(bool on) {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false)) {
    EVT_LOGE("CFG", "config mode write failed: nvs open");
    return;
  }
  if (on) {
    prefs.putBool(kPrefsKey, true);
  } else {
    prefs.remove(kPrefsKey);
  }
  prefs.end();
}

}  // namespace

namespace configmode {

void begin() {
  s_active = readFlag();
  CFG_LOG("[CFG] config_mode=%s", s_active ? "on" : "off");
  EVT_LOGI("CFG", "config_mode=%s", s_active ? "on" : "off");
}

bool active() { return s_active; }

void enter() { writeFlag(true); }

void exit() { writeFlag(false); }

}  // namespace configmode
