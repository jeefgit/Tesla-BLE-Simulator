#pragma once

#include <stddef.h>
#include <stdint.h>

#include "config.h"

// ---------------------------------------------------------------------------
// 设备事件日志（持久化到 LittleFS，可通过 OTA 网页读取）
//
// 设计要点：
//   - log() 只做内存操作（格式化 + 入队），绝不碰文件系统；文件 IO 只在
//     begin/loop/flush/handleCommand/clearAll 里发生。
//   - 文件按槽轮转（默认 4 × 24 KB），写满覆盖最旧，总占用有硬上限。
//   - 关闭 CFG_EVTLOG_ENABLE 时宏退化为空操作；namespace 函数仍有 no-op
//     实现，方便 DashboardApp 无条件调用。
// ---------------------------------------------------------------------------
namespace evtlog {

enum class Level : uint8_t { Info = 0, Warn = 1, Error = 2 };

struct Stats {
  bool mounted;        // 文件系统是否可用
  uint8_t slots;       // 槽总数
  uint32_t slotBytes;  // 单槽上限
  uint32_t usedBytes;  // 所有槽实际占用
  uint32_t seq;        // 当前全局序号（最新一条所在槽的序号）
  uint8_t curSlot;     // 正在写的槽
  uint32_t dropped;    // RAM 缓冲满导致的丢弃行数
  uint32_t writeErrors;
};

struct FileInfo {
  uint8_t slot;
  uint32_t seq;
  uint32_t size;
  bool valid;
};

void begin();  // setup 阶段调用：挂载 + 读 meta + 写开机 banner
void loop();   // 主循环调用：把 RAM 缓冲分块写盘（单次 ≤20ms）
void flush();  // 强制写完（sleep/restart 前）

void log(Level lv, const char* tag, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));
void mark(const char* text);  // 用户标记（来自网页 LOG_MARK）

Stats stats();
uint8_t fileCount();
bool fileInfo(uint8_t index, FileInfo* out);  // index = 0..count-1（按 seq 升序）
int readChunk(uint8_t index, uint32_t offset, uint8_t* dst, size_t len);
void clearAll();

// 串口命令分发：返回 true 表示已处理（由 UsbOta::handleLine 调用）
bool handleCommand(const char* line);
const char* levelName(Level lv);

// OTA 传输期间暂停落盘：log() 仍进 RAM 缓冲，满了照常丢最旧并计数。
void setPaused(bool paused);
bool paused();

}  // namespace evtlog

#if CFG_EVTLOG_ENABLE
#define EVT_LOGI(tag, fmt, ...) \
  ::evtlog::log(::evtlog::Level::Info, tag, fmt, ##__VA_ARGS__)
#define EVT_LOGW(tag, fmt, ...) \
  ::evtlog::log(::evtlog::Level::Warn, tag, fmt, ##__VA_ARGS__)
#define EVT_LOGE(tag, fmt, ...) \
  ::evtlog::log(::evtlog::Level::Error, tag, fmt, ##__VA_ARGS__)
#else
// 关闭时连参数都不求值，避免在 BLE 回调里白算字符串。
#define EVT_LOGI(tag, fmt, ...) \
  do {                          \
  } while (0)
#define EVT_LOGW(tag, fmt, ...) \
  do {                          \
  } while (0)
#define EVT_LOGE(tag, fmt, ...) \
  do {                          \
  } while (0)
#endif
