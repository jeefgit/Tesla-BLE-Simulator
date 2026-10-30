# 特斯拉 BLE 配钥（add-key / add-key-request）

本文说明「真实 Tesla BLE 配钥」在本仓库模拟器与 ESP32 固件里的实现方式，
以及如何在模拟器和真车上操作。协议细节全部对照官方
[`teslamotors/vehicle-command`](https://github.com/teslamotors/vehicle-command)
源码（`pkg/vehicle/security.go`、`pkg/vehicle/vcsec.go`、
`cmd/tesla-control/commands.go`）与本仓库 vendored 的 `.proto`。

---

## 1. 两条配钥路径

| 路径 | 适用场景 | 是否加密 | 是否需要刷卡 |
| --- | --- | --- | --- |
| `add-key-request` | 客户端公钥**尚未**登记 | 否，明文数据报 | 是（中控台轻点 NFC 钥匙卡） |
| `add-key` | 已有 **OWNER** 钥匙，替别人加钥匙 | 是，走正常 ECDH/AES-GCM 会话 | 否 |

### 1.1 add-key-request（未配对引导）

官方实现（`pkg/vehicle/security.go::SendAddKeyRequestWithRole`）：

```go
encodedPayload, _ := proto.Marshal(addKeyPayload(publicKey, role, formFactor))
envelope := &vcsec.ToVCSECMessage{SignedMessage: &vcsec.SignedMessage{
    ProtobufMessageAsBytes: encodedPayload,
    SignatureType:          vcsec.SignatureType_SIGNATURE_TYPE_PRESENT_KEY,
}}
encodedEnvelope, _ := proto.Marshal(envelope)
return v.conn.Send(ctx, encodedEnvelope)   // 明文，不经加密会话
```

要点：

- **`SIGNATURE_TYPE_PRESENT_KEY` 不是 ECDSA 签名**。`SignedMessage` 里根本没有
  签名字段，这个枚举只是告诉 VCSEC「这是钥匙登记请求，需要本地实体授权」。
- 车辆收到后在中控台提示「轻点 NFC 钥匙卡」，先回
  `CommandStatus{operationStatus = OPERATIONSTATUS_WAIT}`；
  刷卡 + UI 确认后再回终态 `CommandStatus{whitelistOperationStatus = ...}`。
- 未配对公钥握手时，车辆不是回协议 fault，而是回
  `SessionInfo{status = SESSION_INFO_STATUS_KEY_NOT_ON_WHITELIST}`，
  官方客户端把它映射为 `ErrKeyNotPaired`。

报文（wire 字段号）：

```
ToVCSECMessage        field 1  signedMessage
  SignedMessage       field 2  protobufMessageAsBytes
                      field 3  signatureType (=2 PRESENT_KEY)
    UnsignedMessage   field 16 WhitelistOperation
      WhitelistOperation
                      field 5  addKeyToWhitelistAndAddPermissions (PermissionChange)
                      field 6  metadataForKey (KeyMetadata)
        PermissionChange
                      field 1  key (PublicKey)
                      field 4  keyRole (Keys.Role: 2=OWNER, 3=DRIVER)
        KeyMetadata   field 1  keyFormFactor (7=ANDROID_DEVICE)
```

### 1.2 add-key（已配对的常规流程）

`UnsignedMessage{WhitelistOperation{addKeyToWhitelistAndAddPermissions}}`
经加密会话发送，由车端 dispatcher 处理，不需要刷卡（见 `tesla_protocol/dispatcher.py`
与 ESP32 的会话命令路径）。

---

## 2. 模拟器侧实现

| 模块 | 作用 |
| --- | --- |
| `tesla_protocol/whitelist.py` | 车端钥匙库：槽位、角色、KeyFormFactor、SHA1(keyId) |
| `tesla_protocol/pairing.py` | add-key-request 明文信封解析 + 刷卡授权状态机 |
| `tesla_protocol/server.py` | 握手时校验白名单、回 `status=1`；接入明文信封；`poll_events()` |
| `tesla_protocol/dispatcher.py` | 加密会话里的 `WhitelistOperation` 增删改查 + OWNER 权限校验 |
| `sim_server.py` | GUI「🔑 轻点钥匙卡」按钮、`--pair-auto`、`--pair-timeout` |

握手行为：

- `--client-pub` 指定白名单（`run.bat` 默认启用）时，**只有白名单里的公钥能握手成功**；
  其他公钥收到 `SessionInfo{status=1}`（无 HMAC tag）。
- 不传 `--client-pub` 时兼容旧行为：任何合法公钥握手后自动登记为 OWNER。

配钥终态 `WhitelistOperation_information_E` 对照（常用）：

| info | 含义 | ESP32 日志 |
| --- | --- | --- |
| 0 | 成功，已写入白名单 | ✅ 配钥成功，重新握手 |
| 4 | 白名单已满 | 需先删除一把钥匙 |
| 5 | 无权添加（当前钥匙非 OWNER） | 用 OWNER 钥匙操作 |
| 13 | 该公钥已在白名单 | 直接重新握手 |
| 24 | 车主拒绝（UI DENIED） | 重新发起或人工授权 |
| 25 | 等待刷卡超时 | 重新发起（`--pair-timeout` 可调） |

### 2.1 操作步骤（模拟器）

1. 正常启动：`run.bat`（带 `--client-pub`，未登记公钥会被拒绝，进入配钥流程）。
2. ESP32 串口出现 `[PAIR] 本机公钥不在车端白名单，准备发送 add-key-request`。
3. 模拟器日志出现 `[PAIR] ⏳ 车辆等待刷卡：pub=… role=OWNER`，
   界面「🔑 轻点钥匙卡」按钮变红。
4. 点击该按钮（等价于中控台刷卡）：
   - 日志 `[PAIR] NFC 卡授权通过 … slot=N`；
   - ESP32 收到终态 `operationStatus=0 info=0`，300ms 后重新握手，随后进入正常遥测轮询。

命令行替代方案：

```bat
py sim_server.py --client-pub <130位hex> --pair-auto
```

`--pair-auto` 收到 add-key-request 立即授权（不模拟刷卡），适合脚本化联调；
`--pair-timeout`（默认 300s）控制等待刷卡超时。

---

## 3. ESP32 固件侧实现

相关文件：

- `include/config.h`：`CFG_TESLA_PAIR_ON_DENIED`、`CFG_TESLA_PAIR_RETRY_MS`、
  `CFG_TESLA_PAIR_MAX_SENDS`、`CFG_TESLA_PAIR_ROLE`、`CFG_TESLA_PAIR_FORM_FACTOR`
- `src/tesla/TeslaCrypto.cpp`：`buildPairingRequest()`、握手 `status=1` 回调、
  未握手时放行 VCSEC 明文状态帧、`parsePairingStatus()`
- `src/tesla/TeslaClient.cpp`：`onNotPaired()` / `sendPairingRequest()` /
  `onPairingStatus()` 调度

流程：

```
连接 → 握手 → 车辆回 status=1
     → 发 add-key-request（明文，最多 CFG_TESLA_PAIR_MAX_SENDS 次）
     → 车辆回 WAIT（串口提示「请在中控台轻点 NFC 钥匙卡」）
     → 刷卡 → 车辆主动回终态 WhitelistOperation_status
     → info=0 → 重新握手 → 会话建立 → GetVehicleData 轮询
```

固件不会在未握手时解析任何业务数据（`ready()` 仍为 false，仪表保持降级显示），
仅放行 VCSEC 域的配钥状态帧。

换新钥匙的完整步骤：

1. 修改 `include/secrets/tesla_client_key.h` 的私钥（或删除该文件改用随机密钥）；
2. 串口会打印新的 65 字节公钥 `pub=...`；
3. 若要让模拟器拒绝该公钥并走配钥流程，重新生成 `run.bat` 里的 `CLIENT_PUB`
   （指向**旧**钥匙）或直接留空/改成一个占位公钥；
4. 烧录（用户自行执行）→ 连接模拟器 → 自动 add-key-request → 界面刷卡授权；
5. 授权后同一会话即可正常握手。

---

## 4. 真车操作参考

真车必须用官方工具或自行实现相同的明文信封，且 **BLE 只能走 VCSEC 域**。

### 4.1 生成密钥对（三种方式，任选其一）

**方式 A：官方 `tesla-keygen`（推荐，私钥存系统钥匙串）**

```bash
export TESLA_KEY_NAME=$(whoami)      # Windows PowerShell: $env:TESLA_KEY_NAME="$env:USERNAME"
export TESLA_VIN=<你的 VIN>          # Windows PowerShell: $env:TESLA_VIN="<你的 VIN>"

tesla-keygen create > public_key.pem                          # 生成私钥（写钥匙串）+ 公钥 PEM
tesla-keygen export > private_key.pem                         # 需要导出私钥时（EC PRIVATE KEY PEM）
```

> 注意：`tesla-control` **没有** `generate-key` 子命令；密钥生成由同仓库的
> `tesla-keygen` 工具完成（`create|delete|export|migrate`）。

**方式 B：本地 Python 直接生成原始密钥（本仓库/ESP32 用这种）**

ESP32 需要的是 32 字节私钥 hex（`CFG_TESLA_CLIENT_KEY_HEX`），公钥是
`0x04 || X || Y` 的 65 字节（130 位 hex）：

```bash
py -3 -c "from cryptography.hazmat.primitives.asymmetric import ec; from cryptography.hazmat.primitives import serialization as s; k=ec.generate_private_key(ec.SECP256R1()); print(f'{k.private_numbers().private_value:064X}'); print(k.public_key().public_bytes(s.Encoding.X962, s.PublicFormat.UncompressedPoint).hex())"
```

第一行私钥 → 写入 `TESLA-DASHBOARD/include/secrets/tesla_client_key.h`；
第二行公钥 → 就是需要登记到车机的 65 字节公钥（固件启动时也会打印 `pub=...`）。

**方式 C：OpenSSL（Git Bash / WSL）**

```bash
openssl ecparam -name prime256v1 -genkey -noout -out private_key.pem
openssl ec -in private_key.pem -pubout -out public_key.pem
```

**把官方 PEM 转成 ESP32 需要的 hex**（方式 A/C 生成后转换）：

```bash
py -3 -c "from cryptography.hazmat.primitives import serialization as s; k=s.load_pem_private_key(open('private_key.pem','rb').read(),None); print(f'{k.private_numbers().private_value:064X}'); print(k.public_key().public_bytes(s.Encoding.X962,s.PublicFormat.UncompressedPoint).hex())"
```

三种方式生成的密钥对在协议层完全等价（P-256 / prime256v1），区别只在存储形式：
官方工具放系统钥匙串，本仓库直接写 hex 进固件。

### 4.2 发起配钥请求

```bash
# 未配对引导：车上中控台会提示「轻点 NFC 钥匙卡」，刷卡后钥匙写入白名单
tesla-control -ble add-key-request public_key.pem owner android_device

# 已配对的 OWNER 钥匙远程加钥（不需要刷卡）
tesla-control -ble -key-file private_key.pem add-key public_key.pem driver android_device

# 查看白名单（不需要私钥） / 删除某把钥匙（需要 OWNER 私钥）
tesla-control -ble list-keys
tesla-control -ble -key-file private_key.pem remove-key public_key.pem
```

`ROLE`：`owner | driver | fm | vehicle_monitor | charging_manager`
`FORM_FACTOR`：`nfc_card | ios_device | android_device | cloud_key`

`add-key-request` 的 `PUBLIC_KEY` 参数可以是**公钥文件**，也可以是**私钥文件**
（工具会自行推导公钥）。

### 4.3 用 ESP32 直接给真车配钥

本仓库固件已经内置同一条官方流程，可以不借助电脑：

1. 把方式 B 生成的私钥写入 `include/secrets/tesla_client_key.h` 并烧录（烧录请自行执行）；
2. 串口确认打印 `[CRYPTO] 客户端密钥就绪 pub=...`，记下这串公钥；
3. 车辆通电、中控屏保持亮起，ESP32 连接车机 BLE；
4. 固件握手被拒后自动发 `add-key-request`（日志：`[PAIR] 已发送 add-key-request ...`）；
5. 按车机提示在中控台轻点 NFC 钥匙卡；
6. 固件收到终态 `✅ 配钥成功` 后自动重新握手，进入正常遥测。

> 注意：真车**不存在** `--pair-auto`，授权只能由车主在中控台完成。
> 配钥是不可逆的授权行为；公钥一旦登记，持有对应私钥的设备即可控车。
> 生产环境请把私钥放进 ESP32 的 NVS 加密分区或硬件安全芯片，
> 不要把真实私钥提交进代码仓库（`secrets/` 已在 `.gitignore` 中）。


---

## 5. 快速自测（无需 ESP32）

```bat
py -3 _tmp_crypto_test.py
```

其中：

- `test_pairing_add_key_request()`：未配对握手 `status=1` → 明文信封 → `WAIT`
  → 刷卡 → 终态 `info=0` → 重新握手 HMAC 校验通过；
- `test_encrypted_add_key()`：OWNER 加密 `add-key` 成功；DRIVER 加钥被拒（info=5）。
