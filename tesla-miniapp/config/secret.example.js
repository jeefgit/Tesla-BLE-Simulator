// 客户端私钥模板：复制本文件为 config/secret.js 并填入真实私钥。
// secret.js 已加入 .gitignore，不会提交到仓库。
// 私钥为 64 位十六进制（32 字节 ECDSA P-256），须与车端/模拟器白名单中的
// 公钥（--client-pub）配对，否则握手会被拒绝。
'use strict';

module.exports = {
  CLIENT_KEY_HEX: 'REPLACE_WITH_YOUR_64_HEX_PRIVATE_KEY',
};
