#include "ble/BleLink.h"

#include "tesla/BackendConfig.h"

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
        CFG_LOG("连接准备失败 (%u/%u)", (unsigned)(m_failCount + 1),
                (unsigned)CFG_CONNECT_FAIL_LIMIT);
        if (++m_failCount >= CFG_CONNECT_FAIL_LIMIT) dropTarget();
        m_state = State::Reconnecting;
        m_nextAttemptAt = millis() + CFG_RECONNECT_INTERVAL_MS;
      }
      break;

    case State::Ready:
      if (!m_client || !m_client->isConnected()) {
        CFG_LOG("BLE 连接断开");
        scheduleReconnect();
      }
      break;

    case State::Reconnecting:
      if ((int32_t)(millis() - m_nextAttemptAt) >= 0) {
        // 断链后清除了缓存地址，因此重新扫描获取最新广播地址
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
  if (!m_client) {
    CFG_LOG("创建 BLE 客户端失败");
    return false;
  }
  m_client->setConnectTimeout(CFG_CONNECT_TIMEOUT_MS);
  m_client->setConnectRetries(0);

  m_writeChar = nullptr;
  m_notifyChar = nullptr;
  if (m_client->isConnected()) m_client->disconnect();

  CFG_LOG("连接 %s (%s)...", m_remoteName.c_str(), m_targetAddr.c_str());
  const NimBLEAddress addr(m_targetAddr, m_targetType);
  if (!m_client->connect(addr)) {
    CFG_LOG("BLE 连接失败");
    return false;
  }
  {
    // MTU 在 connect() 内完成交换（NimBLE 默认 exchangeMTU=true）。
    // 这里必须打印实际协商值：setMTU() 只是本地偏好，最终由车端决定。
    const uint16_t mtu = m_client->getMTU();
    CFG_LOG("BLE 已连接，协商 MTU=%u（本地偏好 %u，单次可收 %uB）",
            (unsigned)mtu, (unsigned)NimBLEDevice::getMTU(),
            (unsigned)(mtu > 3 ? mtu - 3 : 0));
    if (mtu <= 23) {
      CFG_LOG("MTU 仍为默认 23（单次仅 20B）：车端大响应会回 fault=25，"
              "仪表会一直停在 SYNCING");
    }
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

  // 对齐官方 vehicle-command（pkg/connector/ble/ble.go:349）：订阅完成后再
  // 确认/请求一次最大 MTU。NimBLE 的 connect() 已自动交换过一次；若这次仍
  // 停在默认 23，说明交换没生效，这里补发一次（车端可能在订阅后才接受）。
  // exchangeMTU() 是异步的，真实协商结果以首帧数据/故障日志打印的 MTU 为准。
  const uint16_t mtuNow = m_client->getMTU();
  if (mtuNow <= 23) {
    if (m_client->exchangeMTU()) {
      CFG_LOG("MTU 仍为默认 23，订阅后补发 MTU=%u 交换请求",
              (unsigned)CFG_MTU);
    } else {
      CFG_LOG("MTU 交换请求发送失败，沿用当前 MTU=%u（单次仅 %uB）",
              (unsigned)mtuNow, (unsigned)(mtuNow > 3 ? mtuNow - 3 : 0));
    }
  } else {
    CFG_LOG("MTU 已协商为 %u，跳过重复交换", (unsigned)mtuNow);
  }

  // 服务发现/订阅后 MTU 可能被协议栈更新，再确认一次。
  CFG_LOG("已订阅通知，协商 MTU=%u（单次通知上限 %uB）",
          (unsigned)m_client->getMTU(),
          (unsigned)(m_client->getMTU() > 3 ? m_client->getMTU() - 3 : 0));

  m_failCount = 0;
  return true;
}

uint16_t BleLink::mtu() const {
  if (!m_client || !m_client->isConnected()) return 0;
  return m_client->getMTU();
}

void BleLink::dropTarget() {
  CFG_LOG("连续连接失败，清除目标 %s，改为重新扫描", m_remoteName.c_str());
  m_hasTarget = false;
  m_targetAddr.clear();
  m_failCount = 0;
}

void BleLink::scheduleReconnect() {
  m_writeChar = nullptr;
  m_notifyChar = nullptr;
  m_hasTarget = false;
  m_targetAddr.clear();
  m_failCount = 0;
  m_state = State::Reconnecting;
  m_nextAttemptAt = millis() + CFG_RECONNECT_INTERVAL_MS;

  if (m_client && m_client->isConnected() && !m_client->disconnect()) {
    CFG_LOG("BLE 主动断开失败，等待链路释放");
  }
}

bool BleLink::send(const char* cmd) {
  if (!cmd) return false;
  String out(cmd);
  out += '\r';
  return sendRaw((const uint8_t*)out.c_str(), out.length());
}

bool BleLink::sendRaw(const uint8_t* data, size_t len) {
  if (!data || len == 0 || m_state != State::Ready) return false;
  if (!m_client || !m_client->isConnected() || !m_writeChar) {
    CFG_LOG("BLE 链路或写特征不可用，启动重连");
    scheduleReconnect();
    return false;
  }

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
    if (!m_writeChar->writeValue(data + off, n, true)) {
      CFG_LOG("BLE GATT 写入失败（%u/%uB，MTU=%u）", (unsigned)off,
              (unsigned)len, (unsigned)mtu);
      scheduleReconnect();
      return false;
    }
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
