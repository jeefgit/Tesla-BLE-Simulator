// M1 加密模块自测：与 Node 官方 crypto 实现交叉对拍（node scripts/selftest.js）
'use strict';

const crypto = require('crypto');
const assert = require('assert');

const {
  sha1, sha256, hmacSha256, gcmEncrypt, gcmDecrypt,
  derivePublicKey, ecdhSharedX,
} = require('../core/crypto/index');
const { advertisedNameFromVin } = require('../core/advertisedName');
const { toHex } = require('../utils/bytes');

function rnd(n) {
  return new Uint8Array(crypto.randomBytes(n));
}

let passed = 0;

// ── SHA-1 / SHA-256 / HMAC-SHA256 随机对拍 ────────────────────────
for (let i = 0; i < 50; i++) {
  const data = rnd(Math.floor(Math.random() * 300));
  const key = rnd(Math.floor(Math.random() * 100) + 1);
  assert.strictEqual(
    toHex(sha1(data)),
    crypto.createHash('sha1').update(Buffer.from(data)).digest('hex'),
    `sha1 case ${i}`
  );
  assert.strictEqual(
    toHex(sha256(data)),
    crypto.createHash('sha256').update(Buffer.from(data)).digest('hex'),
    `sha256 case ${i}`
  );
  assert.strictEqual(
    toHex(hmacSha256(key, data)),
    crypto.createHmac('sha256', Buffer.from(key)).update(Buffer.from(data)).digest('hex'),
    `hmac case ${i}`
  );
  passed += 3;
}
console.log('sha1 / sha256 / hmac-sha256 OK (' + passed + ' cases)');

// ── AES-128-GCM 随机对拍（含空 AAD / 空明文 / 篡改拒绝）──────────
for (let i = 0; i < 100; i++) {
  const key = rnd(16);
  const nonce = rnd(12);
  const aad = rnd(Math.floor(Math.random() * 40));
  const pt = rnd(Math.floor(Math.random() * 130));

  const enc = crypto.createCipheriv('aes-128-gcm', Buffer.from(key), Buffer.from(nonce));
  enc.setAAD(Buffer.from(aad));
  const ctNode = Buffer.concat([enc.update(Buffer.from(pt)), enc.final()]);
  const tagNode = enc.getAuthTag();

  const mine = gcmEncrypt(key, nonce, aad, pt);
  assert.strictEqual(toHex(mine.ciphertext), ctNode.toString('hex'), `gcm ct case ${i}`);
  assert.strictEqual(toHex(mine.tag), tagNode.toString('hex'), `gcm tag case ${i}`);

  const dec = gcmDecrypt(key, nonce, aad, mine.ciphertext, mine.tag);
  assert.strictEqual(toHex(dec), toHex(pt), `gcm dec case ${i}`);

  if (mine.ciphertext.length > 0) {
    const bad = mine.ciphertext.slice();
    bad[0] ^= 1;
    assert.strictEqual(gcmDecrypt(key, nonce, aad, bad, mine.tag), null, `tamper ct case ${i}`);
  }
  const badTag = mine.tag.slice();
  badTag[15] ^= 1;
  assert.strictEqual(gcmDecrypt(key, nonce, aad, mine.ciphertext, badTag), null, `tamper tag case ${i}`);
  passed += 5;
}
console.log('aes-128-gcm OK (incl. tamper rejection)');

// ── P-256：公钥推导 + ECDH 与 Node ECDH 对拍 ─────────────────────
for (let i = 0; i < 20; i++) {
  const a = crypto.createECDH('prime256v1');
  a.generateKeys();
  const b = crypto.createECDH('prime256v1');
  b.generateKeys();
  const privA = new Uint8Array(a.getPrivateKey());
  const privB = new Uint8Array(b.getPrivateKey());
  const pubAHex = a.getPublicKey('hex');
  const pubA = new Uint8Array(a.getPublicKey());
  const pubB = new Uint8Array(b.getPublicKey());

  assert.strictEqual(
    toHex(derivePublicKey(privA)),
    pubAHex,
    `pub derive case ${i}`
  );
  assert.strictEqual(
    toHex(ecdhSharedX(privA, pubB)),
    a.computeSecret(Buffer.from(pubB)).toString('hex'),
    `ecdh A case ${i}`
  );
  assert.strictEqual(
    toHex(ecdhSharedX(privB, pubA)),
    b.computeSecret(Buffer.from(pubA)).toString('hex'),
    `ecdh B case ${i}`
  );
  passed += 3;
}
console.log('p256 derivePublicKey / ecdhSharedX OK');

// ── 广播名推导 ────────────────────────────────────────────────────
const vin = 'LRWYGCFJ8RC571213';
const name = advertisedNameFromVin(vin);
assert.strictEqual(name.length, 18, 'name length');
assert.strictEqual(name[0], 'S');
assert.strictEqual(name[17], 'C');
assert.ok(/^S[0-9a-f]{16}C$/.test(name), 'name pattern');
console.log('advertisedName OK: ' + vin + ' -> ' + name);

console.log('\nALL SELFTESTS PASSED (' + passed + ' assertions)');
