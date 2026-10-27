#include "obd/ObdClient.h"

#include "config.h"

namespace {
const char* const kInitCmds[] = CFG_OBD_INIT_CMDS;
constexpr uint8_t kInitCmdCount = sizeof(kInitCmds) / sizeof(kInitCmds[0]);
}  // namespace

void ObdClient::begin(BleLink* link) {
  m_link = link;
  m_state = State::WaitLink;
  m_lineLen = 0;
}

void ObdClient::feed(const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    const char c = (char)data[i];
    if (c == '\r' || c == '\n' || c == '>') {
      if (m_lineLen > 0) {
        m_line[m_lineLen] = '\0';
        handleLine(m_line);
        m_lineLen = 0;
      }
      if (c == '>') handlePrompt();
    } else if (m_lineLen < CFG_OBD_LINE_MAX - 1) {
      m_line[m_lineLen++] = c;
    }
  }
}

void ObdClient::loop() {
  if (!m_link) return;
  const uint32_t now = millis();

  if (!m_link->isConnected()) {
    if (m_state != State::WaitLink) CFG_LOG("OBD: 链路断开，等待重连后重新初始化");
    m_state = State::WaitLink;
    m_lineLen = 0;
    return;
  }

  switch (m_state) {
    case State::WaitLink:
      CFG_LOG("OBD: 链路就绪，开始初始化 ELM327");
      m_state = State::Init;
      m_initIdx = 0;
      sendInitCmd();
      break;

    case State::Init:
      // 收到 '>' 会把 m_initSentAt 提前，从而立即进入下一条
      if (now - m_initSentAt >= CFG_OBD_INIT_INTERVAL_MS) {
        if (++m_initIdx >= kInitCmdCount) {
          CFG_LOG("OBD: 初始化完成，开始轮询 %u 个 PID", (unsigned)OBD_PID_COUNT);
          m_state = State::Poll;
          m_nextPollAt = now;
        } else {
          sendInitCmd();
        }
      }
      break;

    case State::WaitResponse:
      if (now - m_requestAt >= CFG_OBD_RESPONSE_TIMEOUT_MS) {
        CFG_LOG("OBD: [%02X%02X] 响应超时", m_pendingMode, m_pendingPid);
        m_state = State::Poll;
      }
      break;

    case State::Poll:
      if ((int32_t)(now - m_nextPollAt) >= 0) {
        sendRequest(OBD_PID_TABLE[m_pidIdx]);
        m_pidIdx = (uint8_t)((m_pidIdx + 1) % OBD_PID_COUNT);
        m_state = State::WaitResponse;
        m_nextPollAt = now + CFG_OBD_POLL_INTERVAL_MS;
      }
      break;
  }
}

void ObdClient::sendInitCmd() {
  m_initSentAt = millis();
  m_link->send(kInitCmds[m_initIdx]);
  CFG_LOG("OBD AT -> %s", kInitCmds[m_initIdx]);
}

void ObdClient::sendRequest(const ObdPid& pid) {
  char cmd[8];
  snprintf(cmd, sizeof(cmd), "%02X%02X", pid.mode, pid.pid);
  m_pendingMode = pid.mode;
  m_pendingPid = pid.pid;
  m_pendingMetric = pid.metric;
  m_requestAt = millis();
  m_link->send(cmd);
}

void ObdClient::handleLine(const char* line) {
  if (m_state == State::Init) return;  // AT 应答内容无所谓，继续下一条
  if (m_state != State::WaitResponse) return;

  // 明确的失败 / 空响应
  if (line[0] == '?' || strstr(line, "NO DATA") || strstr(line, "UNABLE") ||
      strstr(line, "CAN ERROR") || strstr(line, "DATA ERROR") ||
      strstr(line, "STOPPED")) {
    CFG_LOG("OBD: [%02X%02X] 应答 %s", m_pendingMode, m_pendingPid, line);
    m_state = State::Poll;
    return;
  }

  if (parseResponse(line)) m_state = State::Poll;
}

void ObdClient::handlePrompt() {
  if (m_state == State::Init) {
    // 本条 AT 指令执行完毕，下一轮 loop 立即发下一条
    m_initSentAt = millis() - CFG_OBD_INIT_INTERVAL_MS;
  } else if (m_state == State::WaitResponse) {
    // 有提示符但没有可解析的行，结束本次请求
    m_state = State::Poll;
  }
}

bool ObdClient::parseResponse(const char* line) {
  // 抽取所有十六进制字符，丢掉地址/报文头/空格/换行等干扰
  char hex[CFG_OBD_LINE_MAX];
  size_t n = 0;
  for (const char* p = line; *p != '\0' && n < sizeof(hex) - 1; ++p) {
    const char c = *p;
    if (c >= '0' && c <= '9') hex[n++] = c;
    else if (c >= 'A' && c <= 'F') hex[n++] = c;
    else if (c >= 'a' && c <= 'f') hex[n++] = (char)(c - 'a' + 'A');
  }
  hex[n] = '\0';
  if (n < 4) return false;

  // 找 "模式+0x40+PID"，例如车速请求 010D 的应答是 410Dxx
  char want[5];
  snprintf(want, sizeof(want), "%02X%02X",
           (unsigned)(uint8_t)(m_pendingMode + 0x40u), (unsigned)m_pendingPid);
  const char* pos = strstr(hex, want);
  if (!pos) return false;

  const size_t dataStart = (size_t)(pos - hex) + 4;
  if (dataStart >= n) return false;

  uint8_t bytes[8];
  size_t count = 0;
  for (size_t i = dataStart; i + 1 < n && count < sizeof(bytes); i += 2) {
    char pair[3] = {hex[i], hex[i + 1], '\0'};
    bytes[count++] = (uint8_t)strtoul(pair, nullptr, 16);
  }
  if (count == 0) return false;

  applyValue(m_pendingMetric, bytes, count);
  return true;
}

void ObdClient::applyValue(ObdMetric metric, const uint8_t* bytes, size_t count) {
  switch (metric) {
    case ObdMetric::Speed:
      if (count < 1) return;
      g_vehicle.speedKmh = (float)bytes[0];
      g_vehicle.mark(VF_SPEED);
      break;
    case ObdMetric::Rpm:
      if (count < 2) return;
      g_vehicle.rpm = (uint16_t)(((uint32_t)bytes[0] * 256u + bytes[1]) / 4u);
      g_vehicle.mark(VF_RPM);
      break;
    case ObdMetric::Throttle:
      if (count < 1) return;
      g_vehicle.throttlePct = bytes[0] * 100.0f / 255.0f;
      g_vehicle.mark(VF_THROTTLE);
      break;
    case ObdMetric::Coolant:
      if (count < 1) return;
      g_vehicle.coolantC = (float)bytes[0] - 40.0f;
      g_vehicle.mark(VF_COOLANT);
      break;
    case ObdMetric::Intake:
      if (count < 1) return;
      g_vehicle.intakeC = (float)bytes[0] - 40.0f;
      g_vehicle.mark(VF_INTAKE);
      break;
    case ObdMetric::BattVoltage:
      if (count < 2) return;
      g_vehicle.battVoltage = ((uint32_t)bytes[0] * 256u + bytes[1]) / 1000.0f;
      g_vehicle.mark(VF_BATT_V);
      break;
  }
  g_vehicle.updatedAt = millis();
}
