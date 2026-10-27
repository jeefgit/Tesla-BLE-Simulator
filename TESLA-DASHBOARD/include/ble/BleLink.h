#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>

#include <functional>
#include <string>

// ---------------------------------------------------------------------------
// BleLink：BLE 链路层（ESP32 作为 Central / Client）
//
// 职责：扫描目标盒子 -> 连接 -> 订阅通知特征 -> 收发字节流。
// 对上层只暴露 send() 和 setRxHandler()，BLE 细节全部封在这里。
//
// 线程说明：
//   - send()/loop() 运行在 Arduino loop 任务
//   - BLE 数据回调运行在 NimBLE 协议栈任务，只往环形缓冲里写字节，
//     由 loop() 取出后才调用 RxHandler，所以上层回调是安全的。
// ---------------------------------------------------------------------------
class BleLink {
 public:
  enum class State : uint8_t {
    Idle,         // 尚未开始
    Scanning,     // 正在扫描
    Connecting,   // 正在连接
    Ready,        // 已连接、已订阅，可以收发
    Reconnecting, // 断线等待重连
  };

  // 收到 BLE 数据的回调（在 loop 上下文执行）
  using RxHandler = std::function<void(const uint8_t* data, size_t len)>;

  void begin();
  void loop();

  void setRxHandler(RxHandler h) { m_rx = std::move(h); }

  // 精确目标广播名（特斯拉按 VIN 推导: S + sha1(VIN)[:16] + C）。
  // 配置后扫描只认这个名字，找不到才退回前缀/服务 UUID 兜底。
  void setTargetName(const String& name) { m_targetName = name; }
  const String& targetName() const { return m_targetName; }

  // 发送 ASCII 指令，自动补 '\r'
  bool send(const char* cmd);
  bool send(const String& cmd) { return send(cmd.c_str()); }
  // 发送原始字节
  bool sendRaw(const uint8_t* data, size_t len);

  State state() const { return m_state; }
  bool isConnected() const {
    return m_state == State::Ready && m_client && m_client->isConnected();
  }
  const String& remoteName() const { return m_remoteName; }

 private:
  // NimBLE 回调是 C 风格函数指针，用静态实例回到成员函数
  static BleLink* s_instance;
  static constexpr size_t kRxRingSize = 512;  // 环形缓冲大小（取模实现，任意正整数均可）
  void onBleBytes(const uint8_t* data, size_t len);
  void drainRx();

  bool scanForTarget();
  bool connectAndSubscribe();
  void dropTarget();

  State m_state = State::Idle;
  RxHandler m_rx;

  NimBLEClient* m_client = nullptr;
  NimBLERemoteCharacteristic* m_writeChar = nullptr;
  NimBLERemoteCharacteristic* m_notifyChar = nullptr;

  // 目标设备信息（重连时不再扫描）
  std::string m_targetAddr;
  uint8_t m_targetType = 0;
  bool m_hasTarget = false;
  uint8_t m_failCount = 0;
  String m_targetName;  // 精确广播名（可空 = 走前缀兜底）

  String m_remoteName;
  uint32_t m_nextAttemptAt = 0;

  // 单生产者(NIMBLE任务)单消费者(loop) 环形缓冲
  uint8_t m_rxRing[kRxRingSize];
  volatile uint16_t m_rxHead = 0;
  volatile uint16_t m_rxTail = 0;
  volatile bool m_rxOverflow = false;
};
