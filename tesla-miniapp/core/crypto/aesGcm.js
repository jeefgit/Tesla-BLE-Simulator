// AES-128-GCM 纯 JS 实现（会话加密帧，nonce 固定 12 字节，tag 16 字节）
// S-box 等表在加载时程序化生成，避免手抄 256 项常量出错。
'use strict';

// ── GF(2^8) 乘法表与 S-box 程序化生成 ─────────────────────────────
function rotl8(x, n) { return ((x << n) | (x >>> (8 - n))) & 0xff; }

const SBOX = (function build() {
  const log = new Uint8Array(256);
  const exp = new Uint8Array(255);
  let x = 1;
  for (let i = 0; i < 255; i++) {
    exp[i] = x;
    log[x] = i;
    // x *= 3 (生成元 3)：x*2 带约简再异或 x
    const hi = (x & 0x80) ? 0x1b : 0;
    x = x ^ (((x << 1) & 0xff) ^ hi);
    x &= 0xff;
  }
  const box = new Uint8Array(256);
  for (let i = 0; i < 256; i++) {
    const b = i === 0 ? 0 : exp[(255 - log[i]) % 255];
    box[i] = b ^ rotl8(b, 1) ^ rotl8(b, 2) ^ rotl8(b, 3) ^ rotl8(b, 4) ^ 0x63;
  }
  return box;
})();

function xtime(x) {
  return ((x << 1) ^ ((x & 0x80) ? 0x1b : 0)) & 0xff;
}

// ── AES-128 密钥扩展与块加密 ──────────────────────────────────────
function expandKey128(key) {
  // key: Uint8Array(16) → 11 组轮密钥（176 字节展开）
  const w = new Uint8Array(176);
  w.set(key);
  let rcon = 1;
  for (let i = 16; i < 176; i += 4) {
    if (i % 16 === 0) {
      // RotWord + SubWord + Rcon（每 4 个字一次）
      const t0 = SBOX[w[i - 3]] ^ rcon;
      const t1 = SBOX[w[i - 2]];
      const t2 = SBOX[w[i - 1]];
      const t3 = SBOX[w[i - 4]];
      w[i] = w[i - 16] ^ t0;
      w[i + 1] = w[i - 15] ^ t1;
      w[i + 2] = w[i - 14] ^ t2;
      w[i + 3] = w[i - 13] ^ t3;
      rcon = xtime(rcon);
    } else {
      w[i] = w[i - 16] ^ w[i - 4];
      w[i + 1] = w[i - 15] ^ w[i - 3];
      w[i + 2] = w[i - 14] ^ w[i - 2];
      w[i + 3] = w[i - 13] ^ w[i - 1];
    }
  }
  return w;
}

// 块加密：in/out 均 Uint8Array(16)，roundKeys 为 expandKey128 结果
function encryptBlock(roundKeys, input, output) {
  const s = new Uint8Array(16);
  for (let i = 0; i < 16; i++) s[i] = input[i] ^ roundKeys[i];

  const t = new Uint8Array(16);
  for (let round = 1; round <= 10; round++) {
    // SubBytes + ShiftRows（按列主序的 AES 状态布局）
    for (let i = 0; i < 16; i++) t[i] = SBOX[s[i]];
    const r = t.slice();
    for (let i = 0; i < 16; i++) s[i] = r[(i + (i % 4) * 4) % 16];
    if (round < 10) {
      // MixColumns
      for (let c = 0; c < 4; c++) {
        const o = c * 4;
        const a0 = s[o], a1 = s[o + 1], a2 = s[o + 2], a3 = s[o + 3];
        s[o] = xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3;
        s[o + 1] = a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3;
        s[o + 2] = a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3);
        s[o + 3] = (xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3);
      }
    }
    const rk = round * 16;
    for (let i = 0; i < 16; i++) s[i] ^= roundKeys[rk + i];
  }
  output.set(s);
}

// ── GCM ───────────────────────────────────────────────────────────
const R128 = BigInt('0xe1000000000000000000000000000000');

function gfMult128(x, y) {
  // NIST SP 800-38D 6.3：块左（高）位是 x^0 系数 → x_i 对应 BigInt bit(127-i)。
  // V 右移（BigInt >>1）实现乘 x，溢出（v127 = BigInt LSB）时异或 R。
  let z = 0n;
  let v = y;
  for (let i = 0; i < 128; i++) {
    if (((x >> BigInt(127 - i)) & 1n) === 1n) z = z ^ v;
    if ((v & 1n) === 1n) v = (v >> 1n) ^ R128;
    else v = v >> 1n;
  }
  return z;
}

function bytesToBig(b, off, len) {
  let v = 0n;
  for (let i = 0; i < len; i++) v = (v << 8n) | BigInt(b[off + i]);
  return v;
}

function bigToBytes(v, len) {
  const out = new Uint8Array(len);
  for (let i = len - 1; i >= 0; i--) {
    out[i] = Number(v & 0xffn);
    v >>= 8n;
  }
  return out;
}

function ghash(hashH, aad, ct) {
  let y = 0n;
  const step = (bytes) => {
    for (let off = 0; off < bytes.length; off += 16) {
      const block = new Uint8Array(16);
      block.set(bytes.subarray(off, off + 16));
      y = gfMult128(y ^ bytesToBig(block, 0, 16), hashH);
    }
  };
  step(aad);
  step(ct);
  const lens = new Uint8Array(16);
  const dv = new DataView(lens.buffer);
  dv.setUint32(4, (aad.length * 8) >>> 0);
  dv.setUint32(12, (ct.length * 8) >>> 0);
  y = gfMult128(y ^ bytesToBig(lens, 0, 16), hashH);
  return bigToBytes(y, 16);
}

function inc32(counter) {
  const c = counter.slice();
  const dv = new DataView(c.buffer);
  dv.setUint32(12, (dv.getUint32(12) + 1) >>> 0);
  return c;
}

function ctr(roundKeys, j0, data) {
  const out = new Uint8Array(data.length);
  const ks = new Uint8Array(16);
  let ctrBlk = j0.slice();
  for (let off = 0; off < data.length; off += 16) {
    encryptBlock(roundKeys, ctrBlk, ks);
    const n = Math.min(16, data.length - off);
    for (let i = 0; i < n; i++) out[off + i] = data[off + i] ^ ks[i];
    ctrBlk = inc32(ctrBlk);
  }
  return out;
}

function computeTag(roundKeys, H, j0, aad, ct) {
  const s = ghash(H, aad, ct);
  const ej0 = new Uint8Array(16);
  encryptBlock(roundKeys, j0, ej0);
  const tag = new Uint8Array(16);
  for (let i = 0; i < 16; i++) tag[i] = s[i] ^ ej0[i];
  return tag;
}

// AES-128-GCM 加密：key 16B，nonce 12B → { ciphertext, tag }
function gcmEncrypt(key, nonce, aad, plain) {
  if (key.length !== 16) throw new Error('AES-GCM key must be 16 bytes');
  if (nonce.length !== 12) throw new Error('AES-GCM nonce must be 12 bytes');
  const roundKeys = expandKey128(key);
  const hashH = new Uint8Array(16);
  encryptBlock(roundKeys, new Uint8Array(16), hashH);
  const H = bytesToBig(hashH, 0, 16);

  const j0 = new Uint8Array(16);
  j0.set(nonce);
  j0[15] = 1;

  const ct = ctr(roundKeys, inc32(j0), plain);
  const tag = computeTag(roundKeys, H, j0, aad, ct);
  return { ciphertext: ct, tag };
}

// AES-128-GCM 解密（认证失败返回 null）
function gcmDecrypt(key, nonce, aad, ciphertext, tag) {
  if (key.length !== 16) throw new Error('AES-GCM key must be 16 bytes');
  if (nonce.length !== 12) throw new Error('AES-GCM nonce must be 12 bytes');
  const roundKeys = expandKey128(key);
  const hashH = new Uint8Array(16);
  encryptBlock(roundKeys, new Uint8Array(16), hashH);
  const H = bytesToBig(hashH, 0, 16);

  const j0 = new Uint8Array(16);
  j0.set(nonce);
  j0[15] = 1;

  const expect = computeTag(roundKeys, H, j0, aad, ciphertext);
  let diff = 0;
  for (let i = 0; i < 16; i++) diff |= expect[i] ^ tag[i];
  if (diff !== 0) return null;

  return ctr(roundKeys, inc32(j0), ciphertext);
}

module.exports = { gcmEncrypt, gcmDecrypt, expandKey128, encryptBlock };
