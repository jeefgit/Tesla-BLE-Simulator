// 加密门面：协议所需全部原语
'use strict';

const { sha1 } = require('./sha1');
const { sha256 } = require('./sha256');
const { hmacSha256 } = require('./hmac');
const { gcmEncrypt, gcmDecrypt } = require('./aesGcm');
const p256 = require('./p256');

module.exports = {
  sha1,
  sha256,
  hmacSha256,
  gcmEncrypt,
  gcmDecrypt,
  derivePublicKey: p256.derivePublicKey,
  ecdhSharedX: p256.ecdhSharedX,
};
