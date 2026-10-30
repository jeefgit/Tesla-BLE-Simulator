#include "ble/BleLink.h"

#include "config.h"

namespace {
const char* const kTargetNames[] = CFG_TARGET_NAMES;
constexpr size_t kTargetNameCount = sizeof(kTargetNames) / sizeof(kTargetNames[0]);

// 大小写不敏感的名称前缀匹配（盒子名常带后缀，如 OBDII-A1B2）
bool namePrefixMatch(const String& name, const char* prefix) {
  const size_t n = strlen(prefix);
  if (name.length() < n) return false;
  return name.substring(0, n).equalsIgnoreCase(prefix);
}
}  // namespace

BleLink* BleLink::s_instance = nullptr;

void BleLink::begin() {
  s_instance = this;
  NimBLEDevice::init("TESLA-DASH");
  NimBLEDevice::setMTU(CFG_MTU);  // 请求更大的 MTU，53 字节帧一次推完
  m_state = State::Idle;
  CFG_LOG("BLE 协议栈就绪（特斯拉 Central），目标前缀 %u 个", (unsigned)kTargetNameCount);
}

void BleLink::loop() {
  // 1) 先把 BLE 收到的字节交给上层（在 loop 上下文，安全）
  drainRx();

  // 2) 连接状态机
  switch (m_state) {
    case State::Idle:
      m_state = State::Scanning;
      break;

    case State::Scanning:
      if (scanForTarget()) {
        m_state = State::Connecting;
      } else {
        m_state = State::Reconnecting;
        m_nextAttemptAt = millis() + CFG_RECONNECT_INTERVAL_MS;
      }
      break;

    case State::Connecting:
      if (connectAndSubscribe()) {
        m_state = State::Ready;
        CFG_LOG("BLE 已连接: %s，链路就绪", m_remoteName.c_str());
      } else {
        m_state = State::Reconnecting;
        m_nextAttemptAt = millis() + CFG_RECONNECT_INTERVAL_MS;
      }
      break;

    case State::Ready:
      if (!m_client || !m_client->isConnected()) {
        CFG_LOG("BLE 连接断开");
        m_writeChar = nullptr;
        m_notifyChar = nullptr;
        m_state = State::Reconnecting;
        m_nextAttemptAt = millis() + CFG_RECONNECT_INTERVAL_MS;
      }
      break;

    case State::Reconnecting:
      if ((int32_t)(millis() - m_nextAttemptAt) >= 0) {
        // 还记得目标地址就直接重连，否则重新扫描
        m_state = m_hasTarget ? State::Connecting : State::Scanning;
      }
      break;
  }
}

bool BleLink::scanForTarget() {
  CFG_LOG("扫描 BLE 设备 (%d s)...", CFG_SCAN_SECONDS);

  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);
  scan->clearResults();
  // NimBLE 2.x：getResults(duration_ms, is_continue) 启动扫描并阻塞到完成
  // （1.x 的 start() 是阻塞式且单位为秒，2.x 语义已变，不能沿用）
  NimBLEScanResults results =
      scan->getResults((uint32_t)CFG_SCAN_SECONDS * 1000, false);
  const int count = results.getCount();

  // 第一优先级：精确广播名匹配（特斯拉按 VIN 推导，区分大小写）
  if (m_targetName.length() > 0) {
    for (int i = 0; i < count; ++i) {
      const NimBLEAdvertisedDevice* dev = results.getDevice((uint32_t)i);
      if (!dev || !dev->haveName()) continue;
      const String name(dev->getName().c_str());
      if (name != m_targetName) continue;
      m_targetAddr = dev->getAddress().toString();
      m_targetType = (uint8_t)dev->getAddressType();
      m_remoteName = name;
      m_hasTarget = true;
      m_failCount = 0;
      CFG_LOG("找到目标(精确名): %s (%s)", m_remoteName.c_str(),
              m_targetAddr.c_str());
      return true;
    }
  }

  // 第二优先级：名称前缀匹配（未配置精确名时的兜底）
  for (int i = 0; i < count; ++i) {
    const NimBLEAdvertisedDevice* dev = results.getDevice((uint32_t)i);
    if (!dev || !dev->haveName()) continue;
    const String name(dev->getName().c_str());
    for (size_t k = 0; k < kTargetNameCount; ++k) {
      if (!namePrefixMatch(name, kTargetNames[k])) continue;
      m_targetAddr = dev->getAddress().toString();
      m_targetType = (uint8_t)dev->getAddressType();
      m_remoteName = name;
      m_hasTarget = true;
      m_failCount = 0;
      CFG_LOG("找到目标: %s (%s)", m_remoteName.c_str(), m_targetAddr.c_str());
      return true;
    }
  }

  // 第三优先级：广播了目标服务 UUID 的设备
  const NimBLEUUID serviceUuid(CFG_SERVICE_UUID);
  for (int i = 0; i < count; ++i) {
    const NimBLEAdvertisedDevice* dev = results.getDevice((uint32_t)i);
    if (!dev || !dev->isAdvertisingService(serviceUuid)) continue;
    m_targetAddr = dev->getAddress().toString();
    m_targetType = (uint8_t)dev->getAddressType();
    m_remoteName = dev->haveName() ? String(dev->getName().c_str()) : String("(未命名)");
    m_hasTarget = true;
    m_failCount = 0;
    CFG_LOG("按服务 UUID 找到目标: %s (%s)", m_remoteName.c_str(), m_targetAddr.c_str());
    return true;
  }

  CFG_LOG("未找到目标设备，扫描到 %u 个广播", (unsigned)count);
  return false;
}

bool BleLink::connectAndSubscribe() {
  if (!m_hasTarget) return false;

  if (!m_client) m_client = NimBLEDevice::createClient();

  m_writeChar = nullptr;
  m_notifyChar = nullptr;
  if (m_client->isConnected()) m_client->disconnect();

  CFG_LOG("连接 %s (%s)...", m_remoteName.c_str(), m_targetAddr.c_str());
  const NimBLEAddress addr(m_targetAddr, m_targetType);
  if (!m_client->connect(addr)) {
    CFG_LOG("连接失败 (%u/%u)", (unsigned)(m_failCount + 1),
            (unsigned)CFG_CONNECT_FAIL_LIMIT);
    if (++m_failCount >= CFG_CONNECT_FAIL_LIMIT) dropTarget();
    return false;
  }

  NimBLERemoteService* service = m_client->getService(NimBLEUUID(CFG_SERVICE_UUID));
  if (!service) {
    CFG_LOG("未找到服务 %s（UUID 与车端不匹配？改 config.h）", CFG_SERVICE_UUID);
    m_client->disconnect();
    return false;
  }

  m_writeChar = service->getCharacteristic(NimBLEUUID(CFG_CHAR_WRITE_UUID));
  m_notifyChar = service->getCharacteristic(NimBLEUUID(CFG_CHAR_NOTIFY_UUID));
  if (!m_writeChar || !m_notifyChar) {
    CFG_LOG("未找到收发特征值 (write=%s / notify=%s)", CFG_CHAR_WRITE_UUID,
            CFG_CHAR_NOTIFY_UUID);
    m_client->disconnect();
    return false;
  }
  if (!m_notifyChar->canNotify() && !m_notifyChar->canIndicate()) {
    CFG_LOG("通知特征不可用");
    m_client->disconnect();
    return false;
  }

  const bool subscribed = m_notifyChar->subscribe(
      true, [](NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
        if (s_instance) s_instance->onBleBytes(data, len);
      });
  if (!subscribed) {
    CFG_LOG("订阅通知失败");
    m_client->disconnect();
    return false;
  }

  m_failCount = 0;
  return true;
}

void BleLink::dropTarget() {
  CFG_LOG("连续连接失败，清除目标 %s，改为重新扫描", m_remoteName.c_str());
  m_hasTarget = false;
  m_targetAddr.clear();
  m_failCount = 0;
}

bool BleLink::send(const char* cmd) {
  if (!cmd) return false;
  String out(cmd);
  out += '\r';
  return sendRaw((const uint8_t*)out.c_str(), out.length());
}

bool BleLink::sendRaw(const uint8_t* data, size_t len) {
  if (!isConnected() || !m_writeChar || !data || len == 0) return false;

  // 特斯拉协议要求写 00000212 时必须用 write-with-response
  // （tesla_protocol/protobuf/protocol_reference.md §BLE）。即使特征值同时
  // 声明了 WRITE_NO_RSP，也不能降级成 write command，否则车端可能不回包，
  // 表现为「握手/配钥发了但没有任何响应」。

  // BLE 单次写入上限 = MTU - 3。加密帧（RoutableMessage）可达数百字节，
  // 必须按协商后的 MTU 分片写入，车端/模拟器会按长度前缀自行重组。
  const uint16_t mtu = m_client ? m_client->getMTU() : 0;
  size_t chunk = (mtu > 3) ? (size_t)(mtu - 3) : 20;
  if (chunk > len) chunk = len;

  for (size_t off = 0; off < len; off += chunk) {
    const size_t n = (len - off) < chunk ? (len - off) : chunk;
    if (!m_writeChar->writeValue(data + off, n, true)) return false;
  }
  return true;
}

void BleLink::onBleBytes(const uint8_t* data, size_t len) {
  // NimBLE 任务上下文：只入环形缓冲，不做任何解析
  for (size_t i = 0; i < len; ++i) {
    const uint16_t head = m_rxHead;
    const uint16_t next = (uint16_t)((head + 1) % kRxRingSize);
    if (next == m_rxTail) {
      m_rxOverflow = true;
      return;
    }
    m_rxRing[head] = data[i];
    m_rxHead = next;
  }
}

void BleLink::drainRx() {
  uint8_t chunk[64];
  for (;;) {
    size_t n = 0;
    while (n < sizeof(chunk) && m_rxTail != m_rxHead) {
      chunk[n++] = m_rxRing[m_rxTail];
      m_rxTail = (uint16_t)((m_rxTail + 1) % kRxRingSize);
    }
    if (n == 0) break;
    if (m_rx) m_rx(chunk, n);
  }
  if (m_rxOverflow) {
    m_rxOverflow = false;
    CFG_LOG("BLE 接收缓冲溢出，有数据丢失");
  }
}
