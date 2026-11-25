// 特斯拉广播名推导：S + sha1(VIN) 十六进制前 16 位 + C
// 与 sim_server.py / TeslaClient::advertisedNameFromVin 规则一致
'use strict';

const { sha1 } = require('./crypto/index');
const { asciiBytes, toHex } = require('../utils/bytes');

function advertisedNameFromVin(vin) {
  const digest = toHex(sha1(asciiBytes(vin)));
  return 'S' + digest.slice(0, 16) + 'C';
}

module.exports = { advertisedNameFromVin };
