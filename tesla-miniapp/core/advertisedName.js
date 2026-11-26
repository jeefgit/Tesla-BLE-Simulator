// 特斯拉广播名推导
// - 旧命名（模拟器沿用）：S + sha1(VIN) 十六进制前 16 位 + C
//   例：LRWYGCFJ8RC571213 -> S4d78506904eb799eC
// - 新命名（2023-06 起真车使用）：Tesla + VIN 后 6 位
//   例：LRWYGCFJ8RC571213 -> Tesla 571213
// 真车可能只广播其中一种，扫描时两种都要匹配。
'use strict';

const { sha1 } = require('./crypto/index');
const { asciiBytes, toHex } = require('../utils/bytes');

function normalizeVin(vin) {
  return String(vin || '').trim().toUpperCase();
}

function legacyAdvertisedNameFromVin(vin) {
  const digest = toHex(sha1(asciiBytes(normalizeVin(vin))));
  return 'S' + digest.slice(0, 16) + 'C';
}

// 兼容旧接口
function advertisedNameFromVin(vin) {
  return legacyAdvertisedNameFromVin(vin);
}

// 返回需要匹配的全部广播名（新命名 + 旧命名）
function advertisedNamesFromVin(vin) {
  const v = normalizeVin(vin);
  const names = [];
  if (v.length >= 6) names.push('Tesla ' + v.slice(-6));
  names.push(legacyAdvertisedNameFromVin(v));
  return names;
}

module.exports = {
  normalizeVin,
  advertisedNameFromVin,
  legacyAdvertisedNameFromVin,
  advertisedNamesFromVin,
};
