// 字节工具：Uint8Array / ArrayBuffer / hex / 整型互转
'use strict';

const HEX = '0123456789abcdef';

function toHex(bytes) {
  if (!bytes || bytes.length === 0) return '';
  let s = '';
  for (let i = 0; i < bytes.length; i++) {
    s += HEX[bytes[i] >> 4] + HEX[bytes[i] & 0x0f];
  }
  return s;
}

function fromHex(hex) {
  if (!hex || hex.length % 2) return new Uint8Array(0);
  const out = new Uint8Array(hex.length / 2);
  for (let i = 0; i < out.length; i++) {
    out[i] = parseInt(hex.substr(i * 2, 2), 16);
  }
  return out;
}

function asciiBytes(str) {
  const out = new Uint8Array(str.length);
  for (let i = 0; i < str.length; i++) out[i] = str.charCodeAt(i) & 0xff;
  return out;
}

function concat(parts) {
  let n = 0;
  for (const p of parts) n += p.length;
  const out = new Uint8Array(n);
  let off = 0;
  for (const p of parts) {
    out.set(p, off);
    off += p.length;
  }
  return out;
}

function u32be(v) {
  const b = new Uint8Array(4);
  b[0] = (v >>> 24) & 0xff;
  b[1] = (v >>> 16) & 0xff;
  b[2] = (v >>> 8) & 0xff;
  b[3] = v & 0xff;
  return b;
}

function readU32be(b, off) {
  off = off || 0;
  return (((b[off] << 24) | (b[off + 1] << 16) | (b[off + 2] << 8) | b[off + 3]) >>> 0);
}

// 小端 f32 / i32（53B SimPayload 解析用）
function readF32le(b, off) {
  const dv = new DataView(b.buffer, b.byteOffset + off, 4);
  return dv.getFloat32(0, true);
}

function readI32le(b, off) {
  const dv = new DataView(b.buffer, b.byteOffset + off, 4);
  return dv.getInt32(0, true);
}

function bytesEqual(a, b) {
  if (!a || !b || a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; i++) diff |= a[i] ^ b[i];
  return diff === 0;
}

module.exports = {
  toHex,
  fromHex,
  asciiBytes,
  concat,
  u32be,
  readU32be,
  readF32le,
  readI32le,
  bytesEqual,
};
