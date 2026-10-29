#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// ---------------------------------------------------------------------------
// 极简 Protobuf wire-format 编解码器
//
// 只覆盖特斯拉 BLE 协议用到的子集：
//   varint（uint32/int32/bool/enum）、fixed32（float/fixed32）、
//   length-delimited（bytes/string/嵌套 message）。
// 不做 schema 校验，调用方按字段号自行解释。
//
// 参考 wire format：
//   tag = (field_number << 3) | wire_type
//   wire_type: 0 varint, 1 fixed64, 2 length-delimited, 5 fixed32
// ---------------------------------------------------------------------------
namespace pb {

enum WireType : uint32_t {
  WIRE_VARINT = 0,
  WIRE_FIXED64 = 1,
  WIRE_LEN = 2,
  WIRE_FIXED32 = 5,
};

class Writer {
 public:
  Writer(uint8_t* buf, size_t cap) : m_buf(buf), m_cap(cap) {}

  size_t size() const { return m_len; }
  bool ok() const { return m_ok; }
  const uint8_t* data() const { return m_buf; }

  bool putVarint(uint64_t v) {
    while (v >= 0x80) {
      if (!putByte((uint8_t)((v & 0x7F) | 0x80))) return false;
      v >>= 7;
    }
    return putByte((uint8_t)v);
  }

  bool putTag(uint32_t field, uint32_t wire) {
    return putVarint(((uint64_t)field << 3) | (wire & 7));
  }

  bool putUint32(uint32_t field, uint32_t v) {
    return putTag(field, WIRE_VARINT) && putVarint(v);
  }

  bool putInt32(uint32_t field, int32_t v) {
    return putTag(field, WIRE_VARINT) && putVarint((uint64_t)(int64_t)v);
  }

  bool putBool(uint32_t field, bool v) { return putUint32(field, v ? 1u : 0u); }

  bool putFixed32(uint32_t field, uint32_t v) {
    if (!putTag(field, WIRE_FIXED32)) return false;
    return putByte((uint8_t)(v & 0xFF)) &&
           putByte((uint8_t)((v >> 8) & 0xFF)) &&
           putByte((uint8_t)((v >> 16) & 0xFF)) &&
           putByte((uint8_t)((v >> 24) & 0xFF));
  }

  bool putFloat(uint32_t field, float v) {
    uint32_t raw;
    memcpy(&raw, &v, sizeof(raw));
    return putFixed32(field, raw);
  }

  bool putBytes(uint32_t field, const uint8_t* p, size_t n) {
    if (!putTag(field, WIRE_LEN)) return false;
    if (!putVarint(n)) return false;
    if (n == 0) return true;
    if (!p || m_len + n > m_cap) {
      m_ok = false;
      return false;
    }
    memcpy(m_buf + m_len, p, n);
    m_len += n;
    return true;
  }

  bool putString(uint32_t field, const char* s) {
    if (!s) return false;
    return putBytes(field, (const uint8_t*)s, strlen(s));
  }

  bool putMessage(uint32_t field, const Writer& sub) {
    return putBytes(field, sub.data(), sub.size());
  }

  // 空 message（如 GetVehicleData 的 GetChargeState）：tag + 长度 0。
  // proto3 里 oneof 成员即使内容为空，也必须出现才能表达"已设置"。
  bool putEmptyMessage(uint32_t field) {
    return putTag(field, WIRE_LEN) && putVarint(0);
  }

 private:
  bool putByte(uint8_t b) {
    if (m_len >= m_cap) {
      m_ok = false;
      return false;
    }
    m_buf[m_len++] = b;
    return true;
  }

  uint8_t* m_buf;
  size_t m_cap;
  size_t m_len = 0;
  bool m_ok = true;
};

class Reader {
 public:
  Reader() = default;
  Reader(const uint8_t* data, size_t len) : m_data(data), m_len(len) {}

  void reset(const uint8_t* data, size_t len) {
    m_data = data;
    m_len = len;
    m_pos = 0;
  }

  bool atEnd() const { return m_pos >= m_len; }
  size_t remaining() const { return m_len - m_pos; }

  bool next(uint32_t* field, uint32_t* wire) {
    uint64_t tag;
    if (!readVarint(&tag)) return false;
    *field = (uint32_t)(tag >> 3);
    *wire = (uint32_t)(tag & 7);
    return *field != 0;
  }

  bool readVarint(uint64_t* v) {
    uint64_t result = 0;
    uint32_t shift = 0;
    while (m_pos < m_len && shift < 64) {
      const uint8_t b = m_data[m_pos++];
      result |= ((uint64_t)(b & 0x7F)) << shift;
      if ((b & 0x80) == 0) {
        *v = result;
        return true;
      }
      shift += 7;
    }
    return false;
  }

  bool readBytes(const uint8_t** p, size_t* n) {
    uint64_t len;
    if (!readVarint(&len)) return false;
    if (len > m_len - m_pos) return false;
    *p = m_data + m_pos;
    *n = (size_t)len;
    m_pos += (size_t)len;
    return true;
  }

  bool readFixed32(uint32_t* v) {
    if (m_len - m_pos < 4) return false;
    *v = (uint32_t)m_data[m_pos] | ((uint32_t)m_data[m_pos + 1] << 8) |
         ((uint32_t)m_data[m_pos + 2] << 16) |
         ((uint32_t)m_data[m_pos + 3] << 24);
    m_pos += 4;
    return true;
  }

  bool readFloat(float* v) {
    uint32_t raw;
    if (!readFixed32(&raw)) return false;
    memcpy(v, &raw, sizeof(raw));
    return true;
  }

  bool skip(uint32_t wire) {
    switch (wire) {
      case WIRE_VARINT: {
        uint64_t tmp;
        return readVarint(&tmp);
      }
      case WIRE_FIXED64:
        if (m_len - m_pos < 8) return false;
        m_pos += 8;
        return true;
      case WIRE_LEN: {
        const uint8_t* p;
        size_t n;
        return readBytes(&p, &n);
      }
      case WIRE_FIXED32:
        if (m_len - m_pos < 4) return false;
        m_pos += 4;
        return true;
      default:
        return false;
    }
  }

  bool readMessage(Reader* sub) {
    const uint8_t* p;
    size_t n;
    if (!readBytes(&p, &n)) return false;
    sub->reset(p, n);
    return true;
  }

 private:
  const uint8_t* m_data = nullptr;
  size_t m_len = 0;
  size_t m_pos = 0;
};

}  // namespace pb
