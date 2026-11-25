// HMAC-SHA256（握手 session info key + 响应校验）
'use strict';

const { sha256 } = require('./sha256');

const BLOCK = 64;

function hmacSha256(key, msg) {
  let k = key;
  if (k.length > BLOCK) k = sha256(k);
  const pad = new Uint8Array(BLOCK);
  pad.set(k);

  const inner = new Uint8Array(BLOCK + msg.length);
  const outer = new Uint8Array(BLOCK + 32);
  for (let i = 0; i < BLOCK; i++) {
    inner[i] = pad[i] ^ 0x36;
    outer[i] = pad[i] ^ 0x5c;
  }
  inner.set(msg, BLOCK);
  outer.set(sha256(inner), BLOCK);
  return sha256(outer);
}

module.exports = { hmacSha256 };
