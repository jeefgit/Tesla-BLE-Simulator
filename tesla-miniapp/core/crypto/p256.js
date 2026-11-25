// NIST P-256 (secp256r1) ECDH 纯 JS 实现（BigInt）
// 需求：私钥→公钥（dG）、ECDH 共享点 X（d×peerPub）。协议不使用 ECDSA。
// 依赖运行环境支持 BigInt（iOS 14+ / Android 近代 WebView 均支持）。
'use strict';

const P = 0xffffffff00000001000000000000000000000000ffffffffffffffffffffffffn;
const N = 0xffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551n;
const A = P - 3n;
const B = 0x5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604bn;
const GX = 0x6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296n;
const GY = 0x4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5n;

const INF = null; // 无穷远点用 null 表示

function mod(a) {
  const r = a % P;
  return r < 0n ? r + P : r;
}

function modPow(b, e) {
  let result = 1n;
  let base = mod(b);
  let exp = e;
  while (exp > 0n) {
    if ((exp & 1n) === 1n) result = (result * base) % P;
    base = (base * base) % P;
    exp = exp >> 1n;
  }
  return result;
}

function modInv(a) {
  return modPow(a, P - 2n);
}

// 仿射点 {x, y}
function isOnCurve(x, y) {
  if (x < 0n || x >= P || y < 0n || y >= P) return false;
  return mod(y * y - (x * x * x + A * x + B)) === 0n;
}

// ── Jacobian 坐标点乘 ─────────────────────────────────────────────
// J 点 {x, y, z}，z=0 表示无穷远
function jacDouble(pt) {
  if (!pt || pt.z === 0n) return { x: 0n, y: 1n, z: 0n };
  const { x, y, z } = pt;
  const y2 = mod(y * y);
  const s4 = mod(4n * x * y2);
  const y2s = mod(8n * y2);
  const m = mod(3n * x * x + A * mod(z * z) * mod(z * z));
  const nx = mod(m * m - 2n * s4);
  const ny = mod(m * (s4 - nx) - y2s * y2);
  const nz = mod(2n * y * z);
  return { x: nx, y: ny, z: nz };
}

function jacAdd(pt, q) {
  // pt: J 点，q: 仿射点 {x, y}
  if (!pt || pt.z === 0n) return { x: q.x, y: q.y, z: 1n };
  const z1 = pt.z;
  const z1s = mod(z1 * z1);
  const u2 = mod(q.x * z1s);
  const s2 = mod(q.y * z1 * z1s);
  if (pt.x === u2) {
    if (pt.y === s2) return jacDouble(pt);
    return { x: 0n, y: 1n, z: 0n };
  }
  const h = mod(u2 - pt.x);
  const r = mod(s2 - pt.y);
  const h2 = mod(h * h);
  const h3 = mod(h2 * h);
  const nx = mod(r * r - h3 - 2n * h2 * pt.x);
  const ny = mod(r * (h2 * pt.x - nx) - pt.y * h3);
  const nz = mod(h * z1);
  return { x: nx, y: ny, z: nz };
}

function scalarMult(k, point) {
  // point: 仿射 {x, y}，k: BigInt 标量（MSB→LSB double-and-add）
  let acc = { x: 0n, y: 1n, z: 0n }; // 无穷远
  const bits = k.toString(2);
  for (let i = 0; i < bits.length; i++) {
    acc = jacDouble(acc);
    if (bits[i] === '1') acc = jacAdd(acc, point);
  }
  if (!acc || acc.z === 0n) return null;
  const zinv = modInv(acc.z);
  const zinv2 = mod(zinv * zinv);
  return { x: mod(acc.x * zinv2), y: mod(acc.y * zinv2 * zinv) };
}

// ── 对外接口 ──────────────────────────────────────────────────────

// 65 字节非压缩公钥（0x04 || X || Y）；私钥为 32 字节大端
function derivePublicKey(privBytes) {
  const d = bytesToScalar(privBytes);
  if (d <= 0n || d >= N) throw new Error('invalid private key');
  const q = scalarMult(d, { x: GX, y: GY });
  if (!q) throw new Error('scalar multiplication produced infinity');
  return pointToUncompressed(q);
}

// ECDH：返回共享点 X 坐标 32 字节
function ecdhSharedX(privBytes, peerPub65) {
  const d = bytesToScalar(privBytes);
  if (d <= 0n || d >= N) throw new Error('invalid private key');
  if (!peerPub65 || peerPub65.length !== 65 || peerPub65[0] !== 0x04) {
    throw new Error('peer public key must be 65-byte uncompressed');
  }
  const x = u8ToBig(peerPub65, 1, 32);
  const y = u8ToBig(peerPub65, 33, 32);
  if (!isOnCurve(x, y)) throw new Error('peer public key not on curve');
  const q = scalarMult(d, { x, y });
  if (!q) throw new Error('ECDH shared point is infinity');
  return bigToU8(q.x, 32);
}

function bytesToScalar(b) {
  return u8ToBig(b, 0, b.length);
}

function u8ToBig(b, off, len) {
  let v = 0n;
  for (let i = 0; i < len; i++) v = (v << 8n) | BigInt(b[off + i]);
  return v;
}

function bigToU8(v, len) {
  const out = new Uint8Array(len);
  for (let i = len - 1; i >= 0; i--) {
    out[i] = Number(v & 0xffn);
    v >>= 8n;
  }
  return out;
}

function pointToUncompressed(pt) {
  const out = new Uint8Array(65);
  out[0] = 0x04;
  out.set(bigToU8(pt.x, 32), 1);
  out.set(bigToU8(pt.y, 32), 33);
  return out;
}

module.exports = { derivePublicKey, ecdhSharedX };
