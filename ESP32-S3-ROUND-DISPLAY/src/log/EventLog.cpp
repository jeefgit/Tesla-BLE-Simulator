#include "log/EventLog.h"

#if CFG_EVTLOG_ENABLE

#include <Arduino.h>
#include <LittleFS.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_sleep.h>
#include <esp_system.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace evtlog {
namespace {

// ---------------------------------------------------------------------------
// 常量
// ---------------------------------------------------------------------------
constexpr uint8_t kMaxSlots = 8;
constexpr size_t kLineMax = 160;              // 含结尾 '\n' 的硬上限
constexpr size_t kLineContentMax = kLineMax - 1;
constexpr size_t kMsgMax = 120;               // 留给消息的字节数（含省略号）
constexpr size_t kLineBuf = 200;              // 行缓冲；为 seq 位数变化留余量
constexpr size_t kHeaderReadMax = 80;
constexpr uint32_t kMetaMagic = 0x4C545645u;  // 'E' 'V' 'T' 'L'（小端）
constexpr uint16_t kMetaVersion = 1;
constexpr uint32_t kWriteBudgetMs = 20;       // 主循环单轮文件操作预算
constexpr uint32_t kWriteChunkBytes = 512;    // 单轮最多搬运的原始字节
constexpr size_t kPathMax = 64;

// 注意：Arduino FS API 的路径是“相对挂载点”的，mountpoint 由
// LittleFS.begin(..., "/littlefs", ...) 提供；这里写 "/logs/..."，
// 实际 VFS 路径是 /littlefs/logs/...。
const char* const kLogDir = "/logs";
const char* const kMetaPath = "/logs/meta.bin";
const char* const kMetaTmpPath = "/logs/meta.tmp";

struct Meta {
  uint32_t magic;
  uint16_t version;
  uint8_t curSlot;
  uint8_t reserved;
  uint32_t seq;
  uint32_t boot;
  uint32_t crc;
} __attribute__((packed));

struct HeaderInfo {
  bool valid;
  uint8_t slot;
  uint32_t seq;
  uint32_t boot;
  uint32_t size;
};

struct RateState {
  bool valid;
  uint32_t hash;
  uint32_t lastMs;
  uint16_t suppressed;
};

static_assert(CFG_EVTLOG_RAM_BYTES >= 256, "CFG_EVTLOG_RAM_BYTES 太小");
static_assert(CFG_EVTLOG_READ_CHUNK > 0 && CFG_EVTLOG_READ_CHUNK <= 1024,
              "CFG_EVTLOG_READ_CHUNK 超出范围");

// ---------------------------------------------------------------------------
// 状态
// ---------------------------------------------------------------------------
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

bool s_started = false;
bool s_mounted = false;
bool s_paused = false;
uint8_t s_slotCount = 0;
uint8_t s_curSlot = 0;
uint32_t s_seq = 0;
uint32_t s_boot = 0;
uint32_t s_dropped = 0;         // 对外统计的丢弃行数
uint32_t s_pendingDropped = 0;  // 待落盘时补一条汇总的丢弃行数
uint32_t s_writeErrors = 0;
uint32_t s_lastFlushMs = 0;

bool s_fileOpen = false;
uint32_t s_fileSize = 0;
File s_file;

RateState s_rate = {false, 0, 0, 0};

// RAM 环形字节缓冲：只存完整行（每行都以 '\n' 结尾）。
uint8_t s_buf[CFG_EVTLOG_RAM_BYTES];
uint32_t s_head = 0;
uint32_t s_count = 0;

HeaderInfo s_files[kMaxSlots];
uint8_t s_fileCount = 0;

// 串口命令输出 / 读取缓冲都放静态区，避免大栈数组。
char s_out[1024];
uint8_t s_readBuf[CFG_EVTLOG_READ_CHUNK];

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t len) {
  // 标准 IEEE 802.3（反射，多项式 0xEDB88320），与网页端 crc32() 一致。
  for (size_t i = 0; i < len; ++i) {
    uint32_t c = (crc ^ data[i]) & 0xFFu;
    for (int bit = 0; bit < 8; ++bit) {
      c = (c & 1u) ? ((c >> 1) ^ 0xEDB88320u) : (c >> 1);
    }
    crc = (crc >> 8) ^ c;
  }
  return crc;
}

uint32_t crc32Of(const void* data, size_t len) {
  return crc32Update(0xFFFFFFFFu, static_cast<const uint8_t*>(data), len) ^
         0xFFFFFFFFu;
}

void slotPath(uint8_t slot, char* out, size_t cap) {
  snprintf(out, cap, "%s/evt%u.log", kLogDir, static_cast<unsigned>(slot));
}

void formatTime(char* out, size_t cap, uint32_t ms) {
  const uint32_t totalSec = ms / 1000u;
  const uint32_t hours = totalSec / 3600u;  // 超过 24h 继续累加
  const uint32_t minutes = (totalSec / 60u) % 60u;
  const uint32_t seconds = totalSec % 60u;
  const uint32_t millisPart = ms % 1000u;
  snprintf(out, cap, "%02lu:%02lu:%02lu.%03lu",
           static_cast<unsigned long>(hours),
           static_cast<unsigned long>(minutes),
           static_cast<unsigned long>(seconds),
           static_cast<unsigned long>(millisPart));
}

char levelLetter(Level lv) {
  switch (lv) {
    case Level::Warn:
      return 'W';
    case Level::Error:
      return 'E';
    default:
      return 'I';
  }
}

// 返回合法 UTF-8 序列长度；0 表示非法。
size_t utf8SeqLen(const uint8_t* p, size_t avail) {
  const uint8_t c = p[0];
  if (c < 0x80) {
    return 1;
  }
  size_t need = 0;
  if (c >= 0xC2 && c <= 0xDF) {
    need = 2;
  } else if (c >= 0xE0 && c <= 0xEF) {
    need = 3;
  } else if (c >= 0xF0 && c <= 0xF4) {
    need = 4;
  } else {
    return 0;
  }
  if (avail < need) {
    return 0;
  }
  for (size_t i = 1; i < need; ++i) {
    if ((p[i] & 0xC0) != 0x80) {
      return 0;
    }
  }
  // 拒绝过长编码 / 代理区 / 超出 U+10FFFF 的序列。
  if (need == 3) {
    if (c == 0xE0 && p[1] < 0xA0) return 0;
    if (c == 0xED && p[1] >= 0xA0) return 0;
  } else if (need == 4) {
    if (c == 0xF0 && p[1] < 0x90) return 0;
    if (c == 0xF4 && p[1] >= 0x90) return 0;
  }
  return need;
}

// 把消息压成单行：转义 \r \n，非法 UTF-8 与其它控制字符替换为 '?'；
// 放不下时截断并以 UTF-8 省略号结尾。maxOut 含省略号本身。
void sanitizeMessage(const char* in, char* out, size_t maxOut,
                     bool* truncated) {
  const size_t limit = maxOut > 3 ? maxOut - 3 : maxOut;
  const size_t avail = strlen(in);
  size_t o = 0;
  bool full = false;
  for (size_t i = 0; i < avail;) {
    const uint8_t c = static_cast<uint8_t>(in[i]);
    if (c == '\r' || c == '\n') {
      if (o + 2 <= limit) {
        out[o++] = '\\';
        out[o++] = (c == '\r') ? 'r' : 'n';
        ++i;
      } else {
        full = true;
        break;
      }
    } else if (c < 0x20 || c == 0x7F) {
      if (o + 1 <= limit) {
        out[o++] = '?';
        ++i;
      } else {
        full = true;
        break;
      }
    } else if (c < 0x80) {
      if (o + 1 <= limit) {
        out[o++] = static_cast<char>(c);
        ++i;
      } else {
        full = true;
        break;
      }
    } else {
      const size_t need =
          utf8SeqLen(reinterpret_cast<const uint8_t*>(in) + i, avail - i);
      if (need == 0) {
        if (o + 1 <= limit) {
          out[o++] = '?';
          ++i;
        } else {
          full = true;
          break;
        }
      } else if (o + need <= limit) {
        memcpy(out + o, in + i, need);
        o += need;
        i += need;
      } else {
        full = true;
        break;
      }
    }
  }
  if (full && maxOut >= 3) {
    out[o++] = static_cast<char>(0xE2);  // …
    out[o++] = static_cast<char>(0x80);
    out[o++] = static_cast<char>(0xA6);
  }
  out[o] = '\0';
  if (truncated != nullptr) {
    *truncated = full;
  }
}

uint32_t hashText(const char* a, const char* b) {
  uint32_t h = 2166136261u;
  while (*a != '\0') {
    h ^= static_cast<uint8_t>(*a++);
    h *= 16777619u;
  }
  h ^= 0xFFu;
  h *= 16777619u;
  while (*b != '\0') {
    h ^= static_cast<uint8_t>(*b++);
    h *= 16777619u;
  }
  return h;
}

// ---------------------------------------------------------------------------
// RAM 环形缓冲（调用方必须已进入临界区）
// ---------------------------------------------------------------------------
bool dropOldestLineLocked() {
  if (s_count == 0) {
    return false;
  }
  while (s_count > 0) {
    const char c = static_cast<char>(s_buf[s_head]);
    s_head = (s_head + 1u) % CFG_EVTLOG_RAM_BYTES;
    --s_count;
    if (c == '\n') {
      ++s_dropped;
      if (s_pendingDropped != UINT32_MAX) {
        ++s_pendingDropped;
      }
      return true;
    }
  }
  return true;  // 缓冲里只剩残行，已清空
}

void ringPushLocked(const char* data, size_t len) {
  while (s_count + len > CFG_EVTLOG_RAM_BYTES) {
    if (!dropOldestLineLocked()) {
      // 正常不会发生：行本身远小于缓冲容量。
      s_head = 0;
      s_count = 0;
      break;
    }
  }
  uint32_t tail = (s_head + s_count) % CFG_EVTLOG_RAM_BYTES;
  const uint32_t first = CFG_EVTLOG_RAM_BYTES - tail;
  const size_t headLen = (len < first) ? len : first;
  memcpy(s_buf + tail, data, headLen);
  if (len > headLen) {
    memcpy(s_buf, data + headLen, len - headLen);
  }
  s_count += static_cast<uint32_t>(len);
}

size_t popLineLocked(char* out, size_t cap) {
  if (s_count == 0 || cap < 2) {
    return 0;
  }
  size_t n = 0;
  uint32_t idx = s_head;
  while (n < s_count && n + 1 < cap) {
    const char c = static_cast<char>(s_buf[idx]);
    out[n++] = c;
    idx = (idx + 1u) % CFG_EVTLOG_RAM_BYTES;
    if (c == '\n') {
      break;
    }
  }
  s_head = idx;
  s_count -= static_cast<uint32_t>(n);
  out[n] = '\0';
  return n;
}

uint32_t ringCount() {
  portENTER_CRITICAL(&s_mux);
  const uint32_t n = s_count;
  portEXIT_CRITICAL(&s_mux);
  return n;
}

// 挂载失败时也要把缓冲消费掉，否则迟早会满；只计数不落盘。
void drainWithoutDisk(uint32_t maxBytes) {
  portENTER_CRITICAL(&s_mux);
  uint32_t scanned = 0;
  uint32_t lines = 0;
  while (s_count > 0 && scanned < maxBytes) {
    const char c = static_cast<char>(s_buf[s_head]);
    s_head = (s_head + 1u) % CFG_EVTLOG_RAM_BYTES;
    --s_count;
    ++scanned;
    if (c == '\n') {
      ++lines;
    }
  }
  s_dropped += lines;
  portEXIT_CRITICAL(&s_mux);
}

// ---------------------------------------------------------------------------
// 串口镜像 / 行入队
// ---------------------------------------------------------------------------
void mirrorLine(const char* line) {
#if CFG_EVTLOG_SERIAL_MIRROR
#if CFG_DEBUG_LOG
  if (cfglog::muted()) {
    return;
  }
#endif
  Serial.println(line);
#else
  (void)line;
#endif
}

// 把一行（不含 '\n'）写入缓冲；seq 字段在临界区里填写并自增。
void enqueueLine(char* line) {
  size_t len = strlen(line);
  if (len > kLineContentMax) {
    len = kLineContentMax;
    line[len] = '\0';
  }

  portENTER_CRITICAL(&s_mux);
  const uint32_t seq = s_seq + 1u;
  s_seq = seq;

  // 把 "000000" 替换成实际序号；超过 6 位时把后面的内容右移。
  uint32_t tmp = seq;
  int digits = 1;
  while (tmp >= 10u) {
    tmp /= 10u;
    ++digits;
  }
  const int field = digits > 6 ? digits : 6;
  const int delta = field - 6;
  if (delta > 0) {
    // seq 超过 6 位时总长会变长；先裁掉多出来的部分，守住 160 字节硬上限。
    if (len > kLineContentMax - static_cast<size_t>(delta)) {
      len = kLineContentMax - static_cast<size_t>(delta);
      line[len] = '\0';
    }
    // line[7] 是 ']'，line[8] 是分隔空格；只移动其后内容。
    memmove(line + 8 + delta, line + 8, len - 8 + 1);
    len += static_cast<size_t>(delta);
  }
  char dig[12];
  int d = 0;
  tmp = seq;
  do {
    dig[d++] = static_cast<char>('0' + (tmp % 10u));
    tmp /= 10u;
  } while (tmp != 0 && d < 11);
  int pos = 1;
  for (int i = 0; i < field - d; ++i) {
    line[pos++] = '0';
  }
  for (int i = d - 1; i >= 0; --i) {
    line[pos++] = dig[i];
  }
  // 序号超过 6 位时，右移的是 ']' 之后的内容，']' 本身要重新落到新位置；
  // 否则第 7 位数字会把它覆盖掉，整个行前缀就坏了。
  line[pos++] = ']';

  line[len++] = '\n';
  line[len] = '\0';
  ringPushLocked(line, len);
  portEXIT_CRITICAL(&s_mux);
}

void emitLine(Level lv, const char* tag, const char* msg) {
  char ts[24];
  formatTime(ts, sizeof(ts), millis());
  char line[kLineBuf];
  const char* useTag = (tag != nullptr && tag[0] != '\0') ? tag : "-";
  const char* useMsg = (msg != nullptr) ? msg : "";
  int n = snprintf(line, sizeof(line), "[000000] %s %c %-6.6s %s", ts,
                   levelLetter(lv), useTag, useMsg);
  if (n <= 0) {
    return;
  }
  if (static_cast<size_t>(n) >= sizeof(line)) {
    n = static_cast<int>(sizeof(line)) - 1;
    line[n] = '\0';
  }
  if (static_cast<size_t>(n) > kLineContentMax) {
    n = static_cast<int>(kLineContentMax);
    line[n] = '\0';
  }
  enqueueLine(line);
  // enqueueLine 会把换行补进缓冲区；串口镜像用 println，这里先去掉避免空行。
  const size_t l = strlen(line);
  if (l > 0 && line[l - 1] == '\n') {
    line[l - 1] = '\0';
  }
  mirrorLine(line);
}

// 开机 banner 等不带 [seq] 前缀的原始行。
void emitRawLine(const char* text) {
  char line[kLineBuf];
  size_t n = strlen(text);
  if (n > kLineContentMax) {
    n = kLineContentMax;
  }
  memcpy(line, text, n);
  const size_t contentLen = n;
  line[n++] = '\n';
  line[n] = '\0';
  portENTER_CRITICAL(&s_mux);
  ringPushLocked(line, n);
  portEXIT_CRITICAL(&s_mux);
  line[contentLen] = '\0';
  mirrorLine(line);
}

// 缓冲满丢行后，在下一次落盘前补一条汇总（会占一条普通日志的 seq）。
void flushDroppedSummary() {
  uint32_t n = 0;
  portENTER_CRITICAL(&s_mux);
  n = s_pendingDropped;
  s_pendingDropped = 0;
  portEXIT_CRITICAL(&s_mux);
  if (n == 0) {
    return;
  }
  char msg[48];
  snprintf(msg, sizeof(msg), "dropped %lu lines (buffer full)",
           static_cast<unsigned long>(n));
  emitLine(Level::Warn, "LOG", msg);
}

// flush 时把尚未汇总的限频计数补一条，避免计数永远看不到。
void flushSuppressedSummary() {
  uint16_t n = 0;
  portENTER_CRITICAL(&s_mux);
  n = s_rate.suppressed;
  s_rate.suppressed = 0;
  portEXIT_CRITICAL(&s_mux);
  if (n == 0) {
    return;
  }
  char msg[48];
  snprintf(msg, sizeof(msg), "suppressed %u repeats", static_cast<unsigned>(n));
  emitLine(Level::Warn, "LOG", msg);
}

// ---------------------------------------------------------------------------
// meta.bin（临时文件 + rename，掉电时不至于把旧 meta 写坏）
// ---------------------------------------------------------------------------
bool readMeta(Meta* m) {
  File f = LittleFS.open(kMetaPath, "r");
  if (!f) {
    return false;
  }
  const size_t n = f.read(reinterpret_cast<uint8_t*>(m), sizeof(*m));
  f.close();
  if (n != sizeof(*m)) {
    return false;
  }
  if (m->magic != kMetaMagic || m->version != kMetaVersion) {
    return false;
  }
  const uint32_t crc = crc32Of(m, sizeof(*m) - sizeof(m->crc));
  return crc == m->crc;
}

bool writeMeta() {
  if (!s_mounted) {
    return false;
  }
  Meta m;
  memset(&m, 0, sizeof(m));
  m.magic = kMetaMagic;
  m.version = kMetaVersion;
  m.curSlot = s_curSlot;
  m.seq = s_seq;
  m.boot = s_boot;
  m.crc = crc32Of(&m, sizeof(m) - sizeof(m.crc));

  File f = LittleFS.open(kMetaTmpPath, "w");
  if (!f) {
    ++s_writeErrors;
    return false;
  }
  const size_t w = f.write(reinterpret_cast<const uint8_t*>(&m), sizeof(m));
  f.close();
  if (w != sizeof(m)) {
    ++s_writeErrors;
    return false;
  }
  if (!LittleFS.rename(kMetaTmpPath, kMetaPath)) {
    // 某些 core 版本 rename 不覆盖已有文件，退一步删掉再重命名。
    LittleFS.remove(kMetaPath);
    if (!LittleFS.rename(kMetaTmpPath, kMetaPath)) {
      ++s_writeErrors;
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// 槽文件
// ---------------------------------------------------------------------------
bool readHeader(uint8_t slot, HeaderInfo* info) {
  char path[kPathMax];
  slotPath(slot, path, sizeof(path));
  if (!LittleFS.exists(path)) {
    return false;
  }
  File f = LittleFS.open(path, "r");
  if (!f) {
    return false;
  }
  char buf[kHeaderReadMax + 1];
  const size_t n = f.read(reinterpret_cast<uint8_t*>(buf), kHeaderReadMax);
  const uint32_t size = static_cast<uint32_t>(f.size());
  f.close();
  buf[n] = '\0';
  char* nl = strchr(buf, '\n');
  if (nl != nullptr) {
    *nl = '\0';
  }
  unsigned parsedSlot = 0;
  unsigned long parsedSeq = 0;
  unsigned long parsedBoot = 0;
  if (sscanf(buf, "#slot=%u seq=%lu boot=%lu", &parsedSlot, &parsedSeq,
             &parsedBoot) != 3) {
    return false;
  }
  if (parsedSlot > 255u || static_cast<uint8_t>(parsedSlot) != slot) {
    return false;
  }
  info->valid = true;
  info->slot = slot;
  info->seq = static_cast<uint32_t>(parsedSeq);
  info->boot = static_cast<uint32_t>(parsedBoot);
  info->size = size;
  return true;
}

void refreshFiles() {
  s_fileCount = 0;
  for (uint8_t i = 0; i < s_slotCount && i < kMaxSlots; ++i) {
    HeaderInfo h;
    memset(&h, 0, sizeof(h));
    if (!readHeader(i, &h)) {
      continue;
    }
    s_files[s_fileCount++] = h;
  }
  // 按 seq 升序插入排序（最多 8 个元素）。
  for (uint8_t i = 1; i < s_fileCount; ++i) {
    const HeaderInfo key = s_files[i];
    int j = static_cast<int>(i) - 1;
    while (j >= 0 && s_files[j].seq > key.seq) {
      s_files[j + 1] = s_files[j];
      --j;
    }
    s_files[j + 1] = key;
  }
}

void updateCurrentFileSize() {
  for (uint8_t i = 0; i < s_fileCount; ++i) {
    if (s_files[i].slot == s_curSlot) {
      s_files[i].size = s_fileSize;
      return;
    }
  }
}

void closeCurrentFile() {
  if (s_fileOpen) {
    s_file.flush();
    s_file.close();
    s_fileOpen = false;
  }
}

// 以 "w" 打开并写文件头（只在轮转 / 首次创建时调用）。
bool openSlotWrite(uint8_t slot) {
  closeCurrentFile();
  char path[kPathMax];
  slotPath(slot, path, sizeof(path));
  s_file = LittleFS.open(path, "w");
  if (!s_file) {
    return false;
  }
  char header[kHeaderReadMax];
  const int n = snprintf(header, sizeof(header), "#slot=%u seq=%lu boot=%lu\n",
                         static_cast<unsigned>(slot),
                         static_cast<unsigned long>(s_seq),
                         static_cast<unsigned long>(s_boot));
  const size_t w =
      s_file.write(reinterpret_cast<const uint8_t*>(header), static_cast<size_t>(n));
  if (w != static_cast<size_t>(n)) {
    s_file.close();
    s_fileOpen = false;
    LittleFS.remove(path);
    ++s_writeErrors;
    return false;
  }
  s_fileOpen = true;
  s_fileSize = static_cast<uint32_t>(w);
  for (uint8_t i = 0; i < s_fileCount; ++i) {
    if (s_files[i].slot == slot) {
      s_files[i].size = s_fileSize;
      break;
    }
  }
  return true;
}

// 启动 / 恢复时用 "a" 追加，绝不 truncate；文件头无效时才重建。
bool openSlotAppend(uint8_t slot) {
  HeaderInfo h;
  if (!readHeader(slot, &h)) {
    return openSlotWrite(slot);
  }
  closeCurrentFile();
  char path[kPathMax];
  slotPath(slot, path, sizeof(path));
  s_file = LittleFS.open(path, "a");
  if (!s_file) {
    return false;
  }
  s_fileOpen = true;
  s_fileSize = h.size;

  // 掉电可能留下半行：补一个换行，保证后续按行解析不会粘在一起。
  if (h.size > 0) {
    File reader = LittleFS.open(path, "r");
    if (reader) {
      reader.seek(h.size - 1);
      const int last = reader.read();
      reader.close();
      if (last >= 0 && last != '\n') {
        if (s_file.write(reinterpret_cast<const uint8_t*>("\n"), 1) == 1) {
          ++s_fileSize;
        }
      }
    }
  }
  return true;
}

// 轮转到下一个槽：关闭旧文件 → cur++ → seq++ → 写 meta → 新槽写头。
bool rotateSlot() {
  updateCurrentFileSize();
  closeCurrentFile();
  s_curSlot = static_cast<uint8_t>((s_curSlot + 1u) % s_slotCount);
  portENTER_CRITICAL(&s_mux);
  ++s_seq;
  portEXIT_CRITICAL(&s_mux);
  if (!openSlotWrite(s_curSlot)) {
    return false;
  }
  refreshFiles();
  writeMeta();
  return true;
}

uint32_t writeBatch(uint32_t maxBytes, uint32_t budgetMs) {
  uint32_t moved = 0;
  const uint32_t start = millis();
  bool rotated = false;

  while (moved < maxBytes) {
    if (budgetMs != 0 && static_cast<uint32_t>(millis() - start) >= budgetMs) {
      break;
    }
    char line[kLineBuf];
    size_t len;
    portENTER_CRITICAL(&s_mux);
    len = popLineLocked(line, sizeof(line));
    portEXIT_CRITICAL(&s_mux);
    if (len == 0) {
      break;
    }

    if (!s_fileOpen) {
      if (!openSlotAppend(s_curSlot)) {
        ++s_writeErrors;
        s_lastFlushMs = millis();
        break;
      }
    }
    if (s_fileSize > 0 &&
        s_fileSize + len > static_cast<uint32_t>(CFG_EVTLOG_SLOT_BYTES)) {
      if (!rotateSlot()) {
        ++s_writeErrors;
        break;
      }
      rotated = true;
    }
    const size_t w =
        s_file.write(reinterpret_cast<const uint8_t*>(line), len);
    if (w != len) {
      ++s_writeErrors;
      s_lastFlushMs = millis();
      break;
    }
    s_fileSize += static_cast<uint32_t>(len);
    moved += static_cast<uint32_t>(len);
    updateCurrentFileSize();
  }

  if (moved > 0 && s_fileOpen) {
    s_file.flush();
    s_lastFlushMs = millis();
    if (rotated) {
      writeMeta();
    }
  }
  return moved;
}

// ---------------------------------------------------------------------------
// 开机信息
// ---------------------------------------------------------------------------
const char* resetReasonName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:
      return "POWERON";
    case ESP_RST_EXT:
      return "EXT";
    case ESP_RST_SW:
      return "SW";
    case ESP_RST_PANIC:
      return "PANIC";
    case ESP_RST_INT_WDT:
      return "INT_WDT";
    case ESP_RST_TASK_WDT:
      return "TASK_WDT";
    case ESP_RST_WDT:
      return "WDT";
    case ESP_RST_DEEPSLEEP:
      return "DEEP_SLEEP";
    case ESP_RST_BROWNOUT:
      return "BROWNOUT";
    case ESP_RST_SDIO:
      return "SDIO";
    case ESP_RST_USB:
      return "USB";
    case ESP_RST_JTAG:
      return "JTAG";
    case ESP_RST_EFUSE:
      return "EFUSE";
    case ESP_RST_PWR_GLITCH:
      return "POWER_GLITCH";
    case ESP_RST_CPU_LOCKUP:
      return "CPU_LOCKUP";
    default:
      return "UNKNOWN";
  }
}

const char* wakeCauseName(esp_sleep_wakeup_cause_t cause) {
  switch (cause) {
    case ESP_SLEEP_WAKEUP_EXT0:
      return "ext0";
    case ESP_SLEEP_WAKEUP_EXT1:
      return "ext1";
    case ESP_SLEEP_WAKEUP_TIMER:
      return "timer";
    case ESP_SLEEP_WAKEUP_TOUCHPAD:
      return "touchpad";
    case ESP_SLEEP_WAKEUP_ULP:
      return "ulp";
    case ESP_SLEEP_WAKEUP_GPIO:
      return "gpio";
    case ESP_SLEEP_WAKEUP_UART:
      return "uart";
    case ESP_SLEEP_WAKEUP_WIFI:
      return "wifi";
    default:
      return "none";
  }
}

void writeBootBanner() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  const char* part = running != nullptr ? running->label : "?";
  char banner[kLineBuf];
  snprintf(banner, sizeof(banner),
           "===== boot=%lu reset=%s fw=%s build=%s part=%s heap=%lu",
           static_cast<unsigned long>(s_boot),
           resetReasonName(esp_reset_reason()), CFG_FW_VERSION, CFG_FW_BUILD,
           part, static_cast<unsigned long>(ESP.getFreeHeap()));
  emitRawLine(banner);

  const esp_sleep_wakeup_cause_t wake = esp_sleep_get_wakeup_cause();
  if (wake == ESP_SLEEP_WAKEUP_EXT1) {
    EVT_LOGI("BOOT", "wake=%s gpio=%d", wakeCauseName(wake),
             CFG_AUTO_SLEEP_WAKE_GPIO);
  } else {
    EVT_LOGI("BOOT", "wake=%s", wakeCauseName(wake));
  }
}

uint32_t sumUsedBytes() {
  uint32_t used = 0;
  for (uint8_t i = 0; i < s_fileCount; ++i) {
    used += s_files[i].size;
  }
  return used;
}

// ---------------------------------------------------------------------------
// base64
// ---------------------------------------------------------------------------
void base64Encode(const uint8_t* src, size_t len, char* out, size_t outCap) {
  static const char kTable[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t o = 0;
  for (size_t i = 0; i < len && o + 4 < outCap; i += 3) {
    const uint32_t b0 = src[i];
    const uint32_t b1 = (i + 1 < len) ? src[i + 1] : 0;
    const uint32_t b2 = (i + 2 < len) ? src[i + 2] : 0;
    const uint32_t v = (b0 << 16) | (b1 << 8) | b2;
    out[o++] = kTable[(v >> 18) & 0x3F];
    out[o++] = kTable[(v >> 12) & 0x3F];
    out[o++] = (i + 1 < len) ? kTable[(v >> 6) & 0x3F] : '=';
    out[o++] = (i + 2 < len) ? kTable[v & 0x3F] : '=';
  }
  if (o < outCap) {
    out[o] = '\0';
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------
void begin() {
  if (s_started) {
    return;
  }
  s_started = true;
  s_slotCount = CFG_EVTLOG_SLOTS;
  if (s_slotCount < 2) {
    s_slotCount = 2;
  } else if (s_slotCount > kMaxSlots) {
    s_slotCount = kMaxSlots;
  }

  if (!LittleFS.begin(false, "/littlefs", 10, "spiffs")) {
    // 首次使用或掉电导致元数据损坏时格式化一次；仍失败就降级，绝不重启。
    if (!LittleFS.begin(true, "/littlefs", 10, "spiffs")) {
      s_mounted = false;
      ++s_writeErrors;
      Serial.println("[EVTLOG] mount failed; RAM/serial only");
      s_curSlot = 0;
      s_seq = 0;
      s_boot = 1;
      writeBootBanner();
      return;
    }
  }
  s_mounted = true;

  if (!LittleFS.exists(kLogDir)) {
    LittleFS.mkdir(kLogDir);
  }
  refreshFiles();

  Meta meta;
  const bool metaOk = readMeta(&meta) && meta.curSlot < s_slotCount;
  const bool haveFile = s_fileCount > 0;
  const HeaderInfo best = haveFile ? s_files[s_fileCount - 1] : HeaderInfo{};

  if (metaOk) {
    s_curSlot = meta.curSlot;
    s_seq = meta.seq;
    // 掉电可能让 meta 落后于实际文件（meta 只在 flush/轮转时写），
    // 取扫描到的最大 header seq，避免新行序号倒退。
    if (haveFile && best.seq > s_seq) {
      s_seq = best.seq;
    }
  } else if (haveFile) {
    s_curSlot = best.slot;
    s_seq = best.seq;
  } else {
    s_curSlot = 0;
    s_seq = 0;
  }

  uint32_t baseBoot = 0;
  if (metaOk) {
    baseBoot = meta.boot;
  } else if (haveFile) {
    baseBoot = best.boot;
  }
  s_boot = baseBoot + 1u;

  // meta 有效但当前槽文件损坏/丢失时，退回到扫描到的最大 seq 槽。
  HeaderInfo cur;
  memset(&cur, 0, sizeof(cur));
  if (!readHeader(s_curSlot, &cur) && haveFile && best.slot != s_curSlot) {
    HeaderInfo bestHeader;
    memset(&bestHeader, 0, sizeof(bestHeader));
    if (readHeader(best.slot, &bestHeader)) {
      s_curSlot = best.slot;
      s_seq = best.seq;
    }
  }

  if (readHeader(s_curSlot, &cur)) {
    openSlotAppend(s_curSlot);
  } else {
    openSlotWrite(s_curSlot);
  }
  refreshFiles();
  writeMeta();
  s_lastFlushMs = millis();

  writeBootBanner();
}

void loop() {
  if (!s_started) {
    return;
  }
  if (s_paused) {
    return;
  }
  if (!s_mounted) {
    drainWithoutDisk(kWriteChunkBytes);
    return;
  }
  if (ringCount() == 0) {
    return;
  }

  flushDroppedSummary();

  const uint32_t buffered = ringCount();
  const uint32_t now = millis();
  const uint32_t since = static_cast<uint32_t>(now - s_lastFlushMs);
  if (buffered < CFG_EVTLOG_FLUSH_BYTES && since < CFG_EVTLOG_FLUSH_MS) {
    return;  // 攒够一批再写，减少 flash 擦写
  }
  writeBatch(kWriteChunkBytes, kWriteBudgetMs);
}

void flush() {
  if (!s_started) {
    return;
  }
  if (!s_mounted) {
    drainWithoutDisk(ringCount());
    return;
  }
  flushDroppedSummary();
  flushSuppressedSummary();
  // 忽略 20ms 预算：关机/重启前必须把缓冲清干净。
  for (int guard = 0; guard < 1024; ++guard) {
    if (ringCount() == 0) {
      break;
    }
    if (writeBatch(kWriteChunkBytes, 0) == 0) {
      break;
    }
  }
  if (s_fileOpen) {
    s_file.flush();
  }
  writeMeta();
  s_lastFlushMs = millis();
}

void log(Level lv, const char* tag, const char* fmt, ...) {
  if (fmt == nullptr) {
    return;
  }
  if (static_cast<uint8_t>(lv) < CFG_EVTLOG_LEVEL_MIN) {
    return;
  }
  char raw[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(raw, sizeof(raw), fmt, ap);
  va_end(ap);

  char msg[kMsgMax + 4];
  bool truncated = false;
  sanitizeMessage(raw, msg, kMsgMax, &truncated);
  (void)truncated;

  const uint32_t hash = hashText(tag != nullptr ? tag : "", msg);
  const uint32_t now = millis();

  uint16_t repeats = 0;
  bool duplicate = false;
  portENTER_CRITICAL(&s_mux);
  if (s_rate.valid && hash == s_rate.hash &&
      static_cast<uint32_t>(now - s_rate.lastMs) <
          static_cast<uint32_t>(CFG_EVTLOG_RATE_LIMIT_MS)) {
    if (s_rate.suppressed != 0xFFFF) {
      ++s_rate.suppressed;
    }
    duplicate = true;
  } else {
    if (s_rate.valid && s_rate.suppressed > 0) {
      repeats = s_rate.suppressed;
    }
    s_rate.valid = true;
    s_rate.hash = hash;
    s_rate.lastMs = now;
    s_rate.suppressed = 0;
  }
  portEXIT_CRITICAL(&s_mux);

  if (duplicate) {
    return;
  }
  if (repeats > 0) {
    char summary[48];
    snprintf(summary, sizeof(summary), "suppressed %u repeats",
             static_cast<unsigned>(repeats));
    emitLine(Level::Warn, "LOG", summary);
  }
  emitLine(lv, tag, msg);
}

void mark(const char* text) {
  char buf[64];
  const char* src = (text != nullptr) ? text : "";
  // 日志行本身有 160 字节上限，这里再单独限制标记长度，避免挤掉上下文。
  size_t n = strlen(src);
  if (n > 60) {
    n = 60;
  }
  memcpy(buf, src, n);
  buf[n] = '\0';
  log(Level::Info, "MARK", "%s", buf);
}

Stats stats() {
  Stats s;
  memset(&s, 0, sizeof(s));
  s.mounted = s_mounted;
  s.slots = s_slotCount;
  s.slotBytes = CFG_EVTLOG_SLOT_BYTES;
  s.usedBytes = sumUsedBytes();
  s.seq = s_seq;
  s.curSlot = s_curSlot;
  s.dropped = s_dropped;
  s.writeErrors = s_writeErrors;
  return s;
}

uint8_t fileCount() { return s_fileCount; }

bool fileInfo(uint8_t index, FileInfo* out) {
  if (out == nullptr || index >= s_fileCount) {
    return false;
  }
  out->slot = s_files[index].slot;
  out->seq = s_files[index].seq;
  out->size = s_files[index].size;
  out->valid = s_files[index].valid;
  return true;
}

int readChunk(uint8_t index, uint32_t offset, uint8_t* dst, size_t len) {
  if (!s_mounted || dst == nullptr || index >= s_fileCount) {
    return -1;
  }
  const HeaderInfo& h = s_files[index];
  if (offset >= h.size) {
    return 0;
  }
  size_t want = h.size - offset;
  if (len < want) {
    want = len;
  }
  if (want == 0) {
    return 0;
  }
  if (want > CFG_EVTLOG_READ_CHUNK) {
    want = CFG_EVTLOG_READ_CHUNK;
  }
  char path[kPathMax];
  slotPath(h.slot, path, sizeof(path));
  File f = LittleFS.open(path, "r");
  if (!f) {
    return -1;
  }
  if (!f.seek(offset)) {
    f.close();
    return -1;
  }
  const int n = f.read(dst, want);
  f.close();
  return n;
}

void clearAll() {
  portENTER_CRITICAL(&s_mux);
  s_head = 0;
  s_count = 0;
  s_dropped = 0;
  s_pendingDropped = 0;
  s_rate.valid = false;
  s_rate.suppressed = 0;
  portEXIT_CRITICAL(&s_mux);

  s_seq = 0;
  s_curSlot = 0;
  s_fileCount = 0;

  if (!s_mounted) {
    return;
  }

  closeCurrentFile();
  char path[kPathMax];
  for (uint8_t i = 0; i < s_slotCount; ++i) {
    slotPath(i, path, sizeof(path));
    if (LittleFS.exists(path)) {
      LittleFS.remove(path);
    }
  }
  LittleFS.remove(kMetaPath);
  LittleFS.remove(kMetaTmpPath);

  // 清空后不立刻新建日志文件：这样 LOG_INFO 的 used 真正回到 0，符合
  // 网页“清空后 used=0 / 重新读取为空”的验收标准。下一条事件到来时
  // writeBatch() 会自动以 "w" 创建槽文件并写文件头。
  s_fileOpen = false;
  s_fileSize = 0;
  writeMeta();
}

bool handleCommand(const char* line) {
  if (line == nullptr) {
    return false;
  }
  if (strcmp(line, "LOG_INFO") == 0) {
    const Stats s = stats();
    snprintf(s_out, sizeof(s_out),
             "OTA: LOG INFO enabled=%d mount=%s slots=%u slotBytes=%lu "
             "used=%lu seq=%lu cur=%u dropped=%lu err=%lu",
             CFG_EVTLOG_ENABLE, s.mounted ? "ok" : "fail",
             static_cast<unsigned>(s.slots),
             static_cast<unsigned long>(s.slotBytes),
             static_cast<unsigned long>(s.usedBytes),
             static_cast<unsigned long>(s.seq),
             static_cast<unsigned>(s.curSlot),
             static_cast<unsigned long>(s.dropped),
             static_cast<unsigned long>(s.writeErrors));
    Serial.println(s_out);
    return true;
  }

  if (strcmp(line, "LOG_LIST") == 0) {
    for (uint8_t i = 0; i < s_fileCount; ++i) {
      Serial.printf("OTA: LOG FILE slot=%u seq=%lu size=%lu\n",
                    static_cast<unsigned>(s_files[i].slot),
                    static_cast<unsigned long>(s_files[i].seq),
                    static_cast<unsigned long>(s_files[i].size));
    }
    Serial.printf("OTA: LOG LIST done files=%u\n",
                  static_cast<unsigned>(s_fileCount));
    return true;
  }

  if (strncmp(line, "LOG_READ", 8) == 0 &&
      (line[8] == '\0' || line[8] == ' ')) {
    int index = -1;
    unsigned long offset = 0;
    unsigned long len = 0;
    if (sscanf(line, "LOG_READ %d %lu %lu", &index, &offset, &len) != 3 ||
        len == 0) {
      Serial.println("OTA: LOG ERR args");
      return true;
    }
    if (index < 0 || index >= static_cast<int>(s_fileCount)) {
      Serial.println("OTA: LOG ERR index");
      return true;
    }
    if (len > CFG_EVTLOG_READ_CHUNK) {
      len = CFG_EVTLOG_READ_CHUNK;
    }
    FileInfo fi;
    memset(&fi, 0, sizeof(fi));
    fileInfo(static_cast<uint8_t>(index), &fi);
    if (offset > fi.size) {
      Serial.println("OTA: LOG ERR offset");
      return true;
    }
    const int n = readChunk(static_cast<uint8_t>(index),
                            static_cast<uint32_t>(offset), s_readBuf,
                            static_cast<size_t>(len));
    if (n <= 0) {
      Serial.println("OTA: LOG ERR read");
      return true;
    }
    const uint32_t crc = crc32Of(s_readBuf, static_cast<size_t>(n));
    int h = snprintf(s_out, sizeof(s_out),
                     "OTA: LOG DATA slot=%u offset=%lu len=%d crc=%08lx ",
                     static_cast<unsigned>(fi.slot), offset, n,
                     static_cast<unsigned long>(crc));
    if (h < 0) {
      h = 0;
    }
    if (static_cast<size_t>(h) < sizeof(s_out)) {
      base64Encode(s_readBuf, static_cast<size_t>(n), s_out + h,
                   sizeof(s_out) - static_cast<size_t>(h));
    }
    Serial.println(s_out);
    if (static_cast<uint32_t>(offset) + static_cast<uint32_t>(n) >= fi.size) {
      Serial.println("OTA: LOG READ end");
    }
    return true;
  }

  if (strcmp(line, "LOG_CLEAR") == 0) {
    clearAll();
    Serial.println("OTA: LOG CLEAR ok");
    return true;
  }

  if (strncmp(line, "LOG_MARK", 8) == 0 &&
      (line[8] == '\0' || line[8] == ' ')) {
    const char* text = line + 8;
    if (*text == ' ') {
      ++text;
    }
    mark(text);
    Serial.println("OTA: LOG MARK ok");
    return true;
  }

  if (strncmp(line, "LOG_TEST", 8) == 0 &&
      (line[8] == '\0' || line[8] == ' ')) {
#if CFG_EVTLOG_TEST_CMDS
    int count = 0;
    if (sscanf(line, "LOG_TEST %d", &count) != 1 || count <= 0) {
      Serial.println("OTA: LOG ERR args");
      return true;
    }
    if (count > 2000) {
      count = 2000;  // 防止一条命令把主循环卡太久
    }
    for (int i = 0; i < count; ++i) {
      EVT_LOGI("TEST", "tick=%d payload=0123456789abcdefghijklmnopqrstuvwxyz",
               i);
    }
    Serial.printf("OTA: LOG TEST ok %d\n", count);
#else
    Serial.println("OTA: LOG ERR test disabled");
#endif
    return true;
  }

  if (strncmp(line, "LOG_", 4) == 0) {
    Serial.println("OTA: LOG ERR unknown");
    return true;
  }
  return false;
}

const char* levelName(Level lv) {
  switch (lv) {
    case Level::Warn:
      return "W";
    case Level::Error:
      return "E";
    default:
      return "I";
  }
}

void setPaused(bool paused) {
  s_paused = paused;
  if (!paused) {
    // 恢复时让 loop() 立刻尝试把积压的缓冲写出去。
    s_lastFlushMs = millis() - CFG_EVTLOG_FLUSH_MS;
  }
}

bool paused() { return s_paused; }

}  // namespace evtlog

#else  // !CFG_EVTLOG_ENABLE

// 关闭功能时仍提供完整符号，DashboardApp 等调用点无需 #if。
namespace evtlog {

static bool s_paused = false;

void begin() {}
void loop() {}
void flush() {}
void log(Level, const char*, const char*, ...) {}
void mark(const char*) {}
Stats stats() { return Stats{}; }
uint8_t fileCount() { return 0; }
bool fileInfo(uint8_t, FileInfo*) { return false; }
int readChunk(uint8_t, uint32_t, uint8_t*, size_t) { return -1; }
void clearAll() {}
bool handleCommand(const char*) { return false; }
const char* levelName(Level lv) {
  switch (lv) {
    case Level::Warn:
      return "W";
    case Level::Error:
      return "E";
    default:
      return "I";
  }
}
void setPaused(bool paused) { s_paused = paused; }
bool paused() { return s_paused; }

}  // namespace evtlog

#endif
