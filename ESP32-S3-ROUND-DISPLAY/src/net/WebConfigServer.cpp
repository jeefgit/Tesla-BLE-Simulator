#include "net/WebConfigServer.h"
#include "fw_version_api.h"

#if CFG_WEB_CONFIG_ENABLE

#include <Arduino.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

#include <cstdio>
#include <cstring>

#include "config/ConfigMode.h"
#include "config/VinStore.h"
#include "log/EventLog.h"

namespace {

WebServer s_server(CFG_WEB_PORT);

// 单个 HTML 页面，浏览器里完成状态查看、VIN 设置、OTA 上传与日志读取。
// 手机连上热点后打开 http://192.168.4.1 即可。
const char kIndexHtml[] = R"HTML(<!doctype html>
<html lang="zh">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Tesla Dash 配置</title>
<style>
:root{color-scheme:dark}
body{margin:0;font-family:-apple-system,Segoe UI,Roboto,sans-serif;background:#0b0d10;color:#e8eaed;padding:16px;max-width:560px;margin:0 auto}
h1{font-size:20px;margin:8px 0 16px}
.card{background:#16181d;border:1px solid #2a2d33;border-radius:12px;padding:16px;margin-bottom:16px}
.card h2{font-size:15px;margin:0 0 12px;color:#9aa0a6}
.row{display:flex;justify-content:space-between;padding:4px 0;font-size:14px}
.row span:last-child{color:#c9ccd1;font-family:ui-monospace,monospace}
input[type=text]{width:100%;box-sizing:border-box;background:#0f1115;border:1px solid #2a2d33;border-radius:8px;color:#e8eaed;padding:10px;font-size:16px;letter-spacing:.1em}
input[type=file]{width:100%;font-size:13px}
button{background:#e82127;color:#fff;border:0;border-radius:8px;padding:10px 14px;font-size:14px;margin-top:8px;cursor:pointer}
button.ghost{background:#2a2d33}
.msg{margin-top:8px;font-size:13px;color:#9aa0a6}
.msg.ok{color:#5bd389}.msg.err{color:#ff6b6b}
pre{background:#0f1115;border:1px solid #2a2d33;border-radius:8px;padding:12px;font-size:11px;line-height:1.4;overflow:auto;max-height:300px;white-space:pre-wrap;word-break:break-all}
.bar{display:flex;gap:8px;flex-wrap:wrap}
</style>
</head>
<body>
<h1>Tesla Dash 配置</h1>

<div class="card">
<h2>设备状态</h2>
<div class="row"><span>固件版本</span><span id="st-fw">-</span></div>
<div class="row"><span>编译时间</span><span id="st-build">-</span></div>
<div class="row"><span>运行分区</span><span id="st-part">-</span></div>
<div class="row"><span>剩余堆</span><span id="st-heap">-</span></div>
<div class="row"><span>运行时长</span><span id="st-up">-</span></div>
<div class="row"><span>日志占用</span><span id="st-log">-</span></div>
</div>

<div class="card">
<h2>配置模式</h2>
<div class="row"><span>当前为配置模式，BLE 仪表已暂停。</span></div>
<div class="bar"><button class="ghost" onclick="exitConfig()">退出配置模式并重启</button></div>
<div class="msg" id="cfg-msg"></div>
</div>

<div class="card">
<h2>VIN 设置</h2>
<input id="vin" type="text" maxlength="17" placeholder="17 位 VIN" autocomplete="off">
<div class="bar"><button onclick="saveVin()">保存 VIN</button></div>
<div class="msg" id="vin-msg"></div>
</div>

<div class="card">
<h2>固件升级 (OTA)</h2>
<input id="fw" type="file" accept=".bin">
<div class="bar"><button onclick="uploadFw()">上传并升级</button></div>
<div class="msg" id="ota-msg"></div>
</div>

<div class="card">
<h2>日志</h2>
<div class="bar">
<button class="ghost" onclick="loadLogs()">刷新日志</button>
<button class="ghost" onclick="clearLogs()">清空日志</button>
<button class="ghost" onclick="reboot()">重启设备</button>
</div>
<pre id="log">点击“刷新日志”加载…</pre>
</div>

<script>
const $=id=>document.getElementById(id);
async function j(url,opts){const r=await fetch(url,opts);const t=await r.text();try{return JSON.parse(t)}catch(e){return t}}
async function refreshStatus(){
  const s=await j('/api/status');
  $('st-fw').textContent=s.fw||'-';
  $('st-build').textContent=s.build||'-';
  $('st-part').textContent=s.part||'-';
  $('st-heap').textContent=(s.heap||'-')+' B';
  $('st-up').textContent=s.uptime||'-';
  $('st-log').textContent=(s.logUsed||'-')+' / '+(s.logTotal||'-');
  const v=await j('/api/vin');
  $('vin').value=v.vin||'';
}
async function saveVin(){
  const vin=$('vin').value.trim().toUpperCase();
  const m=$('vin-msg');m.className='msg';m.textContent='保存中…';
  const r=await j('/api/vin',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'vin='+encodeURIComponent(vin)});
  if(r.ok){m.className='msg ok';m.textContent='已保存，设备即将重启…'}else{m.className='msg err';m.textContent=r.error||'保存失败'}
}
async function uploadFw(){
  const f=$('fw').files[0];if(!f){const m=$('ota-msg');m.className='msg err';m.textContent='请选择 .bin 文件';return}
  const m=$('ota-msg');m.className='msg';m.textContent='上传中…';
  const fd=new FormData();fd.append('firmware',f);
  const r=await fetch('/update',{method:'POST',body:fd});
  const t=await r.text();
  if(r.ok){m.className='msg ok';m.textContent='升级成功，设备重启中…'}else{m.className='msg err';m.textContent='失败: '+t}
}
async function loadLogs(){$('log').textContent='加载中…';const t=await j('/api/logs/read');$('log').textContent=(typeof t==='string')?t:(t.error||'无日志')}
async function clearLogs(){const r=await j('/api/logs/clear',{method:'POST'});if(r.ok){$('log').textContent='已清空'}else{$('log').textContent='清空失败'}}
async function reboot(){await j('/api/reboot',{method:'POST'});$('log').textContent='设备重启中…'}
async function exitConfig(){const m=$('cfg-msg');m.className='msg';m.textContent='正在退出…';const r=await j('/api/config/exit',{method:'POST'});if(r.ok){m.className='msg ok';m.textContent='已退出，设备重启中…'}else{m.className='msg err';m.textContent='退出失败'}}
refreshStatus();
</script>
</body>
</html>
)HTML";

const char* partLabel(const esp_partition_t* p) {
  return p != nullptr ? p->label : "?";
}

void formatUptime(char* out, size_t cap, uint32_t ms) {
  const uint32_t totalSec = ms / 1000u;
  const uint32_t days = totalSec / 86400u;
  const uint32_t hours = (totalSec / 3600u) % 24u;
  const uint32_t minutes = (totalSec / 60u) % 60u;
  const uint32_t seconds = totalSec % 60u;
  if (days > 0) {
    snprintf(out, cap, "%lud %02lu:%02lu:%02lu",
             static_cast<unsigned long>(days),
             static_cast<unsigned long>(hours),
             static_cast<unsigned long>(minutes),
             static_cast<unsigned long>(seconds));
  } else {
    snprintf(out, cap, "%02lu:%02lu:%02lu",
             static_cast<unsigned long>(hours),
             static_cast<unsigned long>(minutes),
             static_cast<unsigned long>(seconds));
  }
}

}  // namespace

void WebConfigServer::begin() {
  // 热点名带芯片 MAC 后 4 位，避免多台设备同名难区分。
  const uint64_t mac = ESP.getEfuseMac();
  char ssid[32];
  snprintf(ssid, sizeof(ssid), "%s-%04X", CFG_WEB_AP_SSID_PREFIX,
           static_cast<unsigned>(mac & 0xFFFFu));

  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP(ssid, CFG_WEB_AP_PASSWORD, CFG_WEB_AP_CHANNEL)) {
    EVT_LOGE("WEB", "AP start failed");
    return;
  }
  m_started = true;
  registerRoutes();
  s_server.begin();

  CFG_LOG("[WEB] AP ssid=%s ip=%s", ssid,
          WiFi.softAPIP().toString().c_str());
  EVT_LOGI("WEB", "ap=%s ip=%s", ssid, WiFi.softAPIP().toString().c_str());
}

bool WebConfigServer::loop() {
  if (!m_started) {
    return false;
  }
  // 写入 VIN / 升级成功后延时重启：先把响应送出去再复位。
  if (m_restartAt != 0 &&
      static_cast<int32_t>(millis() - m_restartAt) >= 0) {
    evtlog::flush();
    delay(50);
    ESP.restart();
  }
  s_server.handleClient();
  return WiFi.softAPgetStationNum() > 0;
}

void WebConfigServer::registerRoutes() {
  s_server.on("/", HTTP_GET, [this]() { handleRoot(); });

  s_server.on("/api/status", HTTP_GET, [this]() { handleStatus(); });
  s_server.on("/api/vin", HTTP_GET, [this]() { handleVinGet(); });
  s_server.on("/api/vin", HTTP_POST, [this]() { handleVinSet(); });
  s_server.on("/api/logs", HTTP_GET, [this]() { handleLogList(); });
  s_server.on("/api/logs/read", HTTP_GET, [this]() { handleLogRead(); });
  s_server.on("/api/logs/clear", HTTP_POST, [this]() { handleLogClear(); });
  s_server.on("/api/reboot", HTTP_POST, [this]() { handleReboot(); });
  s_server.on("/api/config/exit", HTTP_POST,
              [this]() { handleConfigExit(); });

  // OTA：第一个回调在上传结束后执行，第二个回调按分块处理数据。
  s_server.on("/update", HTTP_POST, [this]() { handleOtaFinish(); },
              [this]() { handleOtaUpload(); });

  s_server.onNotFound([this]() {
    s_server.send(404, "text/plain", "not found");
  });
}

void WebConfigServer::handleRoot() {
  s_server.send(200, "text/html", kIndexHtml);
}

void WebConfigServer::handleStatus() {
  const evtlog::Stats st = evtlog::stats();
  char uptime[32];
  formatUptime(uptime, sizeof(uptime), millis());

  String json;
  json.reserve(256);
  json += "{\"fw\":\"";
  json += fwVersion();
  json += "\",\"build\":\"";
  json += CFG_FW_BUILD;
  json += "\",\"part\":\"";
  json += partLabel(esp_ota_get_running_partition());
  json += "\",\"heap\":";
  json += String(static_cast<unsigned long>(ESP.getFreeHeap()));
  json += ",\"uptime\":\"";
  json += uptime;
  json += "\",\"logUsed\":";
  json += String(static_cast<unsigned long>(st.usedBytes));
  json += ",\"logTotal\":";
  json += String(static_cast<unsigned long>(
      static_cast<uint32_t>(st.slots) * st.slotBytes));
  json += "}";
  s_server.send(200, "application/json", json);
}

void WebConfigServer::handleVinGet() {
  // 这是设备主人自己的车，配置页返回完整 VIN 供编辑；串口 / 日志仍只记尾号。
  String json;
  json.reserve(48);
  json += "{\"vin\":\"";
  json += vinstore::current();
  json += "\",\"default\":";
  json += vinstore::isDefault() ? "true" : "false";
  json += "}";
  s_server.send(200, "application/json", json);
}

void WebConfigServer::handleVinSet() {
  String vin = s_server.arg("vin");
  vin.trim();
  vin.toUpperCase();

  const vinstore::Error err = vinstore::save(vin.c_str());
  if (err != vinstore::Error::None) {
    String json;
    json.reserve(96);
    json += "{\"ok\":false,\"error\":\"";
    json += vinstore::errorText(err);
    json += "\"}";
    s_server.send(200, "application/json", json);
    return;
  }
  // 与设备端编辑器一致：保存成功后延时重启，让 BLE 后端重新读取生效 VIN。
  m_restartAt = millis() + 1200;
  s_server.send(200, "application/json", "{\"ok\":true}");
}

void WebConfigServer::handleLogList() {
  const evtlog::Stats st = evtlog::stats();
  const uint8_t count = evtlog::fileCount();

  String json;
  json.reserve(256);
  json += "{\"mounted\":";
  json += st.mounted ? "true" : "false";
  json += ",\"usedBytes\":";
  json += String(static_cast<unsigned long>(st.usedBytes));
  json += ",\"slotBytes\":";
  json += String(static_cast<unsigned long>(st.slotBytes));
  json += ",\"files\":[";
  for (uint8_t i = 0; i < count; ++i) {
    evtlog::FileInfo fi;
    if (!evtlog::fileInfo(i, &fi)) {
      continue;
    }
    if (i != 0) {
      json += ",";
    }
    json += "{\"slot\":";
    json += String(static_cast<unsigned>(fi.slot));
    json += ",\"seq\":";
    json += String(static_cast<unsigned long>(fi.seq));
    json += ",\"size\":";
    json += String(static_cast<unsigned long>(fi.size));
    json += "}";
  }
  json += "]}";
  s_server.send(200, "application/json", json);
}

void WebConfigServer::handleLogRead() {
  const evtlog::Stats st = evtlog::stats();
  const uint8_t count = evtlog::fileCount();
  if (!st.mounted || count == 0) {
    s_server.send(200, "text/plain", "");
    return;
  }

  // 计算总长度后按序流式返回所有日志文件（按 seq 升序）。
  uint32_t total = 0;
  for (uint8_t i = 0; i < count; ++i) {
    evtlog::FileInfo fi;
    if (evtlog::fileInfo(i, &fi)) {
      total += fi.size;
    }
  }
  s_server.setContentLength(total);
  s_server.send(200, "text/plain", "");

  uint8_t buf[CFG_EVTLOG_READ_CHUNK];
  for (uint8_t i = 0; i < count; ++i) {
    evtlog::FileInfo fi;
    if (!evtlog::fileInfo(i, &fi)) {
      continue;
    }
    uint32_t offset = 0;
    while (offset < fi.size) {
      const int n = evtlog::readChunk(i, offset, buf, sizeof(buf));
      if (n <= 0) {
        break;
      }
      s_server.sendContent(reinterpret_cast<const char*>(buf),
                           static_cast<size_t>(n));
      offset += static_cast<uint32_t>(n);
    }
  }
}

void WebConfigServer::handleLogClear() {
  evtlog::clearAll();
  s_server.send(200, "application/json", "{\"ok\":true}");
}

void WebConfigServer::handleReboot() {
  s_server.send(200, "application/json", "{\"ok\":true}");
  m_restartAt = millis() + 300;
}

void WebConfigServer::handleConfigExit() {
  // 清掉配置模式标志后重启，恢复正常模式（只跑 BLE 仪表）。
  configmode::exit();
  s_server.send(200, "application/json", "{\"ok\":true}");
  m_restartAt = millis() + 300;
}

void WebConfigServer::handleOtaFinish() {
  // 上传结束后统一收尾；失败信息已在分块处理时记录。
  evtlog::setPaused(false);
  if (m_otaError) {
    s_server.send(500, "text/plain", m_otaErrorText);
    evtlog::flush();
    return;
  }
  s_server.sendHeader("Connection", "close");
  s_server.send(200, "text/plain", "OK");
  m_restartAt = millis() + 1500;
}

void WebConfigServer::handleOtaUpload() {
  HTTPUpload& upload = s_server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    m_otaError = false;
    m_otaErrorText[0] = '\0';
    // 升级期间暂停事件落盘，避免与 Update 抢同一片 flash。
    evtlog::setPaused(true);
    EVT_LOGI("OTA", "web start size=%lu",
             static_cast<unsigned long>(upload.totalSize));
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
      m_otaError = true;
      snprintf(m_otaErrorText, sizeof(m_otaErrorText), "begin: %s",
               Update.errorString());
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (!m_otaError &&
        Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      m_otaError = true;
      snprintf(m_otaErrorText, sizeof(m_otaErrorText), "write: %s",
               Update.errorString());
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (!m_otaError && !Update.end(true)) {
      m_otaError = true;
      snprintf(m_otaErrorText, sizeof(m_otaErrorText), "end: %s",
               Update.errorString());
    }
    if (!m_otaError) {
      EVT_LOGI("OTA", "web done part=%s",
               partLabel(esp_ota_get_boot_partition()));
    } else {
      EVT_LOGE("OTA", "web failed: %s", m_otaErrorText);
    }
    evtlog::setPaused(false);
  }
}

#else  // !CFG_WEB_CONFIG_ENABLE

void WebConfigServer::begin() {}
bool WebConfigServer::loop() { return false; }

#endif
