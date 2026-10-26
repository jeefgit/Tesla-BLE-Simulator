# 加密协议完全模拟 — 实施方案 (v1.0)

> 目标读者：接手本项目的开发者 / AI Agent。
> 前置阅读：[HANDOVER.md](HANDOVER.md)（现有 53 字节旁路架构）、[SIMULATOR_IMPROVEMENT_PROMPT.md](SIMULATOR_IMPROVEMENT_PROMPT.md)（命令队列/分域延迟规划）。
> 状态：**方案已定稿，未实施**。ESP32 端选定路线 A（接入 yoziru/tesla-ble 组件 + trust patch）。

---

## 一、背景与目标

当前模拟器通过"53 字节包长旁路"跳过特斯拉 BLE 的全部加解密，ESP32 仪表收到裸 `SimPayload` 直接渲染。这让 HMI 开发零门槛，但 ESP32 固件**从未走过真实车机的加密通信路径**——固件里的握手、解密、Protobuf 代码（或将来要用的 tesla-ble 组件）在模拟器上完全无法验证，上真车才第一次跑，风险后置。

本方案要在模拟器中**按官方规范实现完整的特斯拉 BLE 加密协议服务端**（握手 + ECDH + HMAC/AES-GCM + Protobuf + 域路由），使 ESP32 端可以走 100% 真实的客户端代码路径与模拟器联调。

**可行性依据**：Tesla 官方开源仓库 `teslamotors/vehicle-command` 的 [`pkg/protocol/protocol.md`](https://github.com/teslamotors/vehicle-command/blob/main/pkg/protocol/protocol.md) 提供了完整的协议规范、测试私钥与逐字节测试向量。本方案为照规范实现，非逆向猜测。

### 与现有旁路的关系

**双模式并存，不是替换**：
- **Fast path**：53 字节裸包旁路（现状），保留用于 HMI 快速调试、无加密开销的动画开发；
- **Real path**：完整加密通道（本方案），用于验证 ESP32 真实协议栈代码；
- 运行时按第七节的规则自动分流，GUI 可手动锁定模式。

---

## 二、边界与安全说明（务必先读）

1. **闭环模拟，无法冒充真车**：模拟器使用自签身份或官方公开的**调试测试密钥**，无法生成 Tesla CA 签发的证书链。因此该加密通道仅在「ESP32 ↔ 模拟器」闭环内有效，官方 App 不会接受它。
2. **测试密钥使用限制**：官方文档明确 `vehicle.key` / `client.key` 仅供实现调试与测试向量验证，**禁止**将对应公钥注册到任何真实车辆。
3. **不触碰真车**：本方案所有工作仅针对模拟器与用户自己的 ESP32，与真实车辆无关。
4. **不要把私钥提交进真车相关配置**：模拟器身份密钥应本地生成并加入 `.gitignore`（或固定使用官方测试密钥，因其私钥本就公开）。

---

## 三、协议规范速查（摘自官方 protocol.md，实施时以 vendored 的 .proto 文件为准）

### 3.1 BLE 传输层

| 项目 | 值 |
|---|---|
| Service UUID | `00000211-b2d1-43f0-9b88-960cebf8b91e` |
| Write 特征值（客户端→车） | `00000212-b2d1-43f0-9b88-960cebf8b91e`，**必须使用 write-with-response** |
| Notify/Read 特征值（车→客户端） | `00000213-b2d1-43f0-9b88-960cebf8b91e` |
| 广播名 | `S + <ID> + C`，`<ID>` = SHA1(VIN) 前 8 字节的小写 hex（16 字符）✓ 与现有实现一致 |
| 帧格式 | 每条消息前置 **2 字节大端长度**（长度 = protobuf 部分字节数） |
| MTU | 客户端会协商 MTU；Write 方向大消息需按 `MTU-3` 分块发送，模拟器端必须做**流式重组** |
| 并发限制 | VCSEC 域最多同时维持 3 条 BLE 连接（与钥匙扣/手机钥匙共享） |

### 3.2 消息路由：`UniversalMessage.RoutableMessage`

顶层类型，字段要点：
- `to_destination` / `from_destination`：域路由（`Domain` 枚举）或 16 字节 `routing_address`（客户端随机生成，用于回程关联）；车辆响应时两者对调；
- `payload`（oneof）：`protobuf_message_as_bytes`（应用层负载）| `session_info_request`（握手请求）| `session_info`（握手响应）；
- `signature_data`：认证元数据（HMAC tag 或 GCM tag+nonce）；
- `uuid`：≤16 字节命令唯一标识，回显进响应的 `request_uuid`；
- `flags`：位掩码。客户端**必须**设置 `FLAG_ENCRYPT_RESPONSE`（值 `1<<1 = 2`），指示车辆加密响应（2024.38+ 固件）；
- `signedMessageStatus`：协议层错误（`MessageFault_E`）。

**域（Domains）**：
- `DOMAIN_VEHICLE_SECURITY`（VCSEC）：车锁、车门、后备箱、远程启动等；可独立于 Infotainment 唤醒；
- `DOMAIN_INFOTAINMENT`：充电、空调、遥测、其余一切（应用层为 `CarServer.Action`）。

⚠️ 枚举数值以 `universal_message.proto` 为准（官方示例确认 `DOMAIN_INFOTAINMENT = 0x03`）。

应用层负载：
- 发往 VCSEC 的 → `VCSEC.UnsignedMessage`（vcsec.proto），响应为 `VCSEC.FromVCSECMessage`；
- 发往 Infotainment 的 → `CarServer.Action`（car_server.proto），响应为 `CarServer.Response`。

### 3.3 握手流程（每域独立一套会话）

```
Client                                    Vehicle (模拟器)
  │ RoutableMessage{                        │
  │   to: DOMAIN_X,                         │
  │   session_info_request{                 │
  │     public_key: ENCODE_PUBLIC(C) }  ───►│  C = 客户端 P-256 公钥
  │   uuid }                                │
  │                                    ◄────│ RoutableMessage{
  │                                         │   session_info: SessionInfo{
  │                                         │     counter, publicKey(V),
  │                                         │     epoch(16B), clock_time },
  │                                         │   signature_data.
  │                                         │     session_info_tag{ tag } }
```

- 公钥编码 `ENCODE_PUBLIC(P)` = `0x04 || BIG_ENDIAN(x,32) || BIG_ENDIAN(y,32)`（65 字节非压缩 P-256 点）；
- `SessionInfo` 字段：`counter`（防重放计数器初值）、`publicKey`（车辆 P-256 公钥）、`epoch`（16 字节随机会话纪元）、`clock_time`（该域时钟，秒）。

### 3.4 密钥协商与握手响应认证

```
S = ECDH(c, V)                      # NIST P-256，非 X25519
K = SHA1(BIG_ENDIAN(Sx, 32))[:16]   # 128-bit AES-GCM 会话密钥

SESSION_INFO_KEY = HMAC-SHA256(K, "session info")     # 字符串为 ASCII 字面量
```

客户端验证握手响应（防中间人）：
1. 序列化元数据 `M`（TLV，见 3.5），包含：`TAG_SIGNATURE_TYPE = SIGNATURE_TYPE_HMAC`、`TAG_PERSONALIZATION = VIN`、`TAG_CHALLENGE = 请求的 uuid`，末尾补 `0xFF`；
2. `tag = HMAC-SHA256(SESSION_INFO_KEY, M || session_info_serialized_bytes)`；
3. 与响应中 `session_info_tag.tag` 常量时间比较，不符则丢弃。

### 3.5 元数据 TLV 序列化（认证绑定的核心）

每个键值对编码为 `TAG || LEN || VALUE`，按 **TAG 升序**拼接，末尾追加 `0xFF`。整数一律 4 字节大端。

| Tag 常量 | 编号 | 值 |
|---|---|---|
| `TAG_SIGNATURE_TYPE` | 0 | 签名类型枚举 |
| `TAG_DOMAIN` | 1 | 域枚举 |
| `TAG_PERSONALIZATION` | 2 | **VIN（17 字符）** |
| `TAG_EPOCH` | 3 | `session_info.epoch` |
| `TAG_EXPIRES_AT` | 4 | 按域时钟的过期时间 |
| `TAG_COUNTER` | 5 | 单调递增计数器 |
| `TAG_FLAGS` | 7 | 仅非零时包含（请求）；响应总是包含 |
| `TAG_REQUEST_HASH` / `TAG_FAULT` | 见 `signatures.proto` | 响应解密用 |

签名类型枚举（已确认部分）：`SIGNATURE_TYPE_AES_GCM_PERSONALIZED = 0x05`、`SIGNATURE_TYPE_HMAC = 0x06`；其余（`HMAC_PERSONALIZED`、`AES_GCM_RESPONSE`）数值以 `signatures.proto` 为准。

### 3.6 命令认证（两种模式）

**A. HMAC-SHA256（明文 + 签名，Fleet API 用）**
```
K' = HMAC-SHA256(K, "authenticated command")
tag = HMAC-SHA256(K', M || P)     # P = 应用层 protobuf 明文
```

**B. AES-128-GCM（加密，官方 Go 客户端在 BLE 上用这种 —— 模拟器必须支持）**
- 密钥 = `K`；nonce = **随机 12 字节**；
- AAD = `SHA256(M)`（元数据 TLV 的哈希，不是明文本身）；
- `encrypt(P) → ciphertext || tag(16B)`；
- 密文放入 `payload.protobuf_message_as_bytes`，tag 与 nonce 放入 `signature_data.AES_GCM_Personalized_data`，客户端公钥放入 `signer_identity.public_key`。

**计数器规则**：同 epoch 内每条命令必须递增；VCSEC 要求严格有序，Infotainment 维护滑动窗口容忍乱序。**模拟器需实现该检查**（拒绝重放 → 完整模拟的关键一环）。

### 3.7 响应加密（模拟器 → 客户端）

客户端请求带 `FLAG_ENCRYPT_RESPONSE` 时，模拟器**应**加密响应（模拟 2024.38+ 固件；也可配置成"老固件"回明文以测试客户端兼容分支）：
- 响应元数据：`TAG_SIGNATURE_TYPE = AES_GCM_RESPONSE`、域、VIN、**响应 counter**、**响应 flags**（含位，非零也含）、`TAG_REQUEST_HASH`、`TAG_FAULT`；
- `request_hash` = 1 字节方法标识 + 请求的认证 tag（发往 VCSEC 时截断到 17 字节）；
- 用 nonce + tag + `SHA256(响应元数据)` 作为 AAD 解密负载 → `CarServer.Response` 或 `VCSEC.FromVCSECMessage`。

**计数器验证**：客户端必须验证响应 counter 未在同请求下重复使用（防重放）；一请求多响应时可能乱序（VCSEC 最多 3 条响应）。

### 3.8 会话缓存与错误恢复（模拟器需配合的行为）

- 客户端会缓存 `(K, counter, epoch, 时钟差)`，重连时跳过握手；
- 模拟器**重启后 epoch/counter 会变** → 客户端检测到认证失败后自动重新握手恢复。模拟器要保证该路径可复现（GUI 提供"使会话失效"按钮模拟 Infotainment 重启场景）；
- 车辆可在认证错误响应中携带最新会话状态，引导客户端恢复。

---

## 四、总体架构（双路径）

```
                          ┌────────────────────────────────────────┐
 ESP32 写入 (0x212)  ───► │ 流式重组器 (2字节长度前缀, MTU-3 分块)  │
                          └───────────────┬────────────────────────┘
                                          │ 完整帧
                    ┌─────────────────────┴──────────────────────┐
                    │ 分流判定 (第七节)                           │
          ┌─────────┴─────────┐                    ┌─────────────┴────────────┐
          │ Fast path         │                    │ Real path                │
          │ len==53 且非会话期 │                    │ 长度前缀自洽 且 会话已建立 │
          │ → 现有 SimPayload  │                    │ → RoutableMessage 解析    │
          └─────────┬─────────┘                    │ → 会话/握手状态机         │
                    │                              │ → HMAC 验证 / GCM 解密    │
                    │                              │ → 域路由 dispatcher      │
                    │                              └─────────────┬────────────┘
                    │                                            │
                    │        ┌───────────────────────────────────┤
                    │        ▼                                   ▼
                    │  VCSEC 域应答器                      Infotainment 域应答器
                    │  (车锁/门/后备箱, 300~500ms)          (充电/空调/遥测, 600~1000ms)
                    │        │                                   │
                    │        └───────────┬───────────────────────┘
                    │                    ▼
                    │        响应加密 (可选 FLAG_ENCRYPT_RESPONSE) + 2字节帧
                    │                    │
                    └───────────────► Notify (0x213) ◄────────────┘
                                          │
                     TelemetryState / 剧本引擎 / GUI （现有模块不动，仅扩展）
```

---

## 五、模拟器端设计（Python）

新增包 `tesla_protocol/`，`sim_server.py` 只做薄接入（详见 5.7）。

### 5.1 依赖

```
cryptography>=41.0      # P-256 ECDH、HMAC-SHA256、AESGCM、SHA1/SHA256
protobuf>=3.20.0        # requirements.txt 中已有（作者预留），正式启用
grpcio-tools            # 仅开发期：编译 .proto → Python 绑定
```

### 5.2 vendored proto 文件

从 `teslamotors/vehicle-command/protobuf/` 复制入仓（Apache-2.0，注明来源）：
- `universal_message.proto`（RoutableMessage、Domain、Flags、MessageFault_E）
- `signatures.proto`（SessionInfo、metadata tag、签名类型、AES_GCM_*_data）
- `vcsec.proto`、`car_server.proto`、`car_server_objs.proto`（体积大，只需编译不需手改）
- `io.proto`、`keys.proto`（传递依赖）

编译命令（生成物提交入仓，避免运行期依赖 protoc）：
```powershell
python -m grpc_tools.protoc -I protobuf --python_out=tesla_protocol/pb protobuf/*.proto
```

### 5.3 模块清单

| 模块 | 职责 | 要点 |
|---|---|---|
| `identity.py` | 车辆身份 | 启动生成（或加载）P-256 密钥对；提供 `encode_public()`；支持固定使用官方 `vehicle.key` 测试密钥以便对测试向量 |
| `session.py` | 每域会话状态机 | 状态：`NO_SESSION → HANDSHAKE_SENT → ACTIVE`；保存 per-domain `(K, counter, epoch, clock, client_pubkey, routing_address)`；`derive_k()` = `SHA1(Sx)[:16]`；`verify_session_info_tag()`；counter 递增与校验（VCSEC 有序 / Infotainment 滑窗 32）；会话失效（epoch 轮换） |
| `metadata.py` | TLV 序列化 | 按 tag 升序 + `0xFF` 结尾；构造请求校验元数据 / 响应解密元数据 |
| `secure_link.py` | 帧与密码学 | 2 字节大端长度帧；**Write 流式重组缓冲**（跨 `on_write` 回调拼包，超时 2s 清空防半包悬挂）；HMAC-SHA256 验签；AES-128-GCM encrypt/decrypt（AAD = SHA256(M)）；`request_hash` 计算 |
| `dispatcher.py` | 域路由与业务应答 | 解密后按 `to_destination` 路由；解析 `CarServer.Action` / `VCSEC.UnsignedMessage`；生成对应响应（见 5.5）；无法识别的命令回 `MessageFault_E` / `actionStatus` 错误，不静默丢弃 |
| `latency.py` | 分域延迟注入 | 接入现有 `delay_multiplier` / 丢包 / 抖动参数，按域查延迟表（VCSEC 300~500ms、Infotainment 600~1000ms），随机取值 + 5% ×2 长尾 |

### 5.4 会话状态数据结构（示意）

```python
@dataclass
class DomainSession:
    state: str                 # "none" | "active"
    client_pubkey: bytes       # 65B uncompressed
    shared_key_k: bytes        # 16B
    session_info_key: bytes    # HMAC-SHA256(K, "session info")
    counter: int               # 出方向单调递增
    expected_client_counter: int
    epoch: bytes               # 16B 随机
    clock0: float              # 会话建立时的本地时钟
    routing_address: bytes     # 16B
    recv_window: set[int]      # Infotainment 滑动窗口 (VCSEC 不用)

class TeslaCryptoServer:
    sessions: dict[Domain, DomainSession]   # VCSEC / Infotainment 各一条
    def on_frame(self, frame: bytes) -> list[bytes]   # 返回 0..N 条响应帧
```

### 5.5 域应答器最小命令集（第一期）

只覆盖仪表场景需要的命令，其余回明确错误码：

**VCSEC（车锁/门）**
- `GET_STATUS` → 车辆状态（锁、门、后备箱开口）映射现有 `TelemetryState.data`；
- `GET_WHITELIST_INFO` / `AUTHENTICATE` → 返回固定"已授权 Owner"成功状态（模拟器不做真实白名单，ESP32 端 tesla-ble 启动时会查询）。

**Infotainment（遥测/充电）**
- `GET_VEHICLE_DATA` / `GET_DRIVE_STATE` → 速度、功率、档位（`SimPayload` 字段同源）；
- `GET_CHARGE_STATE` → SOC、续航、充电功率、充电枪状态；
- `GET_CLOSURE_STATE` → 门/锁；
- `GET_CLIMATE_STATE` → 车内/外温度；
- `GET_TIRE_PRESSURE` → 四轮胎压；
- `CHARGE_START / CHARGE_STOP`、`HONK_HORN` 等写命令 → 改 `TelemetryState` 并回 `actionStatus = SUCCESS`（GUI 可见，双向打通）；
- 以上枚举/字段以 `car_server.proto` 为准，实施时对照 `vehicle-command/pkg/vehicle` 文档补齐精确消息定义。

**遥测主动推送**：ESP32 侧订阅车辆主动广播的场景（若 tesla-ble 使用）按其组件文档实现；默认以"问答式"为主，与真实车机一致。

### 5.6 GUI 增强（新卡片：加密会话 CONN SESSION）

- 会话表：两行（VCSEC / Infotainment）× 状态灯、counter、epoch、K 指纹（前 4 字节 hex）、客户端公钥指纹；
- 控件：`加密模式: ON/OFF`（OFF = 回退纯旁路）、`响应加密: 2024.38+ / 老固件`、`使会话失效`（模拟 Infotainment 重启）、`重放攻击演示`（重发上一计数器包 → 客户端应拒绝，用于验证 ESP32）；
- 日志行前缀新增：`[HS]`（握手）、`[GCM]`（加解密）、`[FAULT]`（协议错误）、`[REPLAY]`。

### 5.7 与 sim_server.py 的接入点（改动最小化）

1. `start_ble_server()`：Write 回调从直接打日志改为先喂 `TeslaCryptoServer.on_frame()`，输出帧列表逐条 `notify`（Bless 后端的 `write_request_func` 语义天然匹配 write-with-response）；
2. REALISTIC/IDEAL 推送逻辑保留，仅在 `len==53` fast path 下生效；real path 下由问答驱动；
3. `TelemetryState` 不改结构，仅新增 `crypto_sessions` 只读状态供 GUI 轮询。

---

## 六、ESP32 端设计（路线 A：yoziru/tesla-ble 组件）

### 6.1 集成

```cmake
# main/idf_component.yml（或 CMakeLists.txt REQUIRES）
dependencies:
  tesla-ble: "^0.1"   # github.com/yoziru/tesla-ble
```

组件职责：P-256 ECDH、会话建立与缓存、AES-GCM、Protobuf 编解码、NFC 白名单配对、VCSEC/CarServer 命令收发——全部复用，不自研。

### 6.2 Trust patch（模拟器信任开关）

tesla-ble 常规流程会用内嵌的 Tesla 根公钥验证车辆身份；模拟器无 Tesla CA 证书链，需加编译开关：

```
# Kconfig / sdkconfig
CONFIG_TESLA_SIM_TRUST=y        # 仅模拟器构建使用
CONFIG_TESLA_SIM_VIN="5YJ3E1EA0KF000000"
```

实现方式二选一（实施时对组件版本选型）：
- **方案 a（侵入小）**：patch 组件的证书/公钥校验函数，`SIM_TRUST` 开启时信任固定测试公钥（模拟器启动时打印，用户填入 menuconfig）；
- **方案 b（TOFU）**：首连记录公钥指纹并持久化 NVS，之后固定信任（更接近真车"手机钥匙已配对"语义，改动在应用层）。

⚠️ 该开关**只存在于模拟器调试构建**，量产/真车构建必须关闭。

### 6.3 适配层双路径分流（判定规则升级）

不能只看 `len==53`——真实加密帧也可能恰好 53 字节。**长度前缀自洽性**是更可靠的判据：

```c
bool looks_like_crypto_frame(const uint8_t *d, uint16_t len) {
    if (len < 2) return false;
    uint16_t inner = (d[0] << 8) | d[1];          // 2字节大端
    return (uint16_t)(inner + 2) == len;          // 帧长自洽
}

if (crypto_session_active && looks_like_crypto_frame(d, len))  → real path (tesla-ble)
else if (len == 53 && !crypto_session_active)                  → fast path (SimPayload)
else                                                            → 丢弃并计数日志
```

`vehicle_state_t` 统一状态层保留：real path 解密出的 `CarServer.Response` 同样映射进它，**UI 层完全无感**。53B 旁路与加密路径二选一编译或运行期自动分流均可，推荐运行期分流（同一固件可同时服务两种模拟器模式）。

### 6.4 ESP-IDF 配置要点

- `CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=256`（或 517），保证响应通知不被截断；
- NimBLE GATT client 订阅 0x213 notify；写 0x212 用 **write-with-response**（`ble_gattc_write_flat` 回调式）；
- Write 分块：tesla-ble 组件自带分块逻辑时直接用；否则应用层按 `MTU-3` 切片顺序写、模拟器侧重组（第五节已覆盖）；
- 会话缓存落 NVS（组件支持则开启），验证"模拟器重启 → 自动重握手"恢复路径。

---

## 七、双路径分流判定（汇总）

| 收到字节 | 判定 | 处理 |
|---|---|---|
| `len≥2` 且 `be16(d[0:2]) + 2 == len` 且会话活跃 | 加密帧 | tesla-ble real path |
| `len == 53` 且会话未激活 | SimPayload | 现有旁路解析 |
| 两者冲突（理论上仅当车速 float 前两字节恰构成自洽长度） | 会话优先 | real path |
| 其他 | 未知 | 丢弃 + 日志计数 |

GUI 手动开关可强制单一模式，排除歧义用于测试。

---

## 八、与 SIMULATOR_IMPROVEMENT_PROMPT.md 的关系（红利）

加密上线后，原规划中"因为看不懂请求而做不了"的部分全部解锁：

| 原规划项 | 解锁方式 |
|---|---|
| 问答模式（Phase 1） | 握手后天然问答：收到加密命令才回包 |
| 命令队列 32 深度 + 拥堵（Phase 2） | 能解析命令类型，可按序排队、满 32 拒绝 |
| 分域延迟（Phase 4） | VCSEC 300~500ms / Infotainment 600~1000ms 随机 + 5% 长尾 |
| 按类型响应（Phase 4） | Drive/Charge/Closures/Climate/TPMS 分包回复，不再一包打尽 |
| GUI 队列/延迟显示 | `queue_depth` 终于有真实数据源 |

---

## 九、分阶段实施与验收

| 阶段 | 内容 | 验收标准 |
|---|---|---|
| **P0 基建** | vendored protos + protoc 绑定 + `identity/metadata` + TLV 单测 | TLV 序列化输出与官方示例 hex 逐字节一致（见附录） |
| **P1 握手** | `session.py`：ECDH、K、SESSION_INFO_KEY、session_info_tag 生成 | 用附录测试向量：K、SESSION_INFO_KEY、tag 全部匹配 |
| **P2 安全链路** | `secure_link.py`：帧、流重组、GCM、request_hash | Python 自收发 GCM 回环；附录 AES-GCM 向量匹配 |
| **P3 域应答** | `dispatcher.py` + 5.5 最小命令集 | **用官方 `tesla-control -vin <VIN> -ble` CLI 连模拟器**完成握手 + list-keys + 读状态（免 ESP32 先行验证） |
| **P4 拟真** | `latency.py` 分域延迟 + 命令队列 + GUI 会话卡片 | 压测脚本复现实车拥堵日志形态（队列 29/32 等） |
| **P5 ESP32** | tesla-ble 接入 + trust patch + 双路径分流 | 仪表在加密模式下全功能回归；重放演示按钮被 ESP32 正确拒绝；模拟器重启后自动重握手 |
| **P6 回归** | fast path 53B 旁路全量回归 | 现有 IDEAL/REALISTIC 模式行为与 v2.0 一致 |

**P3 的隐藏测试神器**：官方 `vehicle-command` 仓库自带 `tesla-control` 命令行客户端（P-256 + AES-GCM 完整实现），模拟器一完成即可用它当现成测试对端，无需等 ESP32。

**P1/P2 阶段离线单测优先**——附录测试向量让加解密核心可以不连任何蓝牙就验证到 100% 正确。

---

## 十、风险清单与开放问题

| # | 风险 / 开放问题 | 影响 | 对策 |
|---|---|---|---|
| 1 | Notify 响应超过 MTU-3 的分片策略未在官方文档说明 | 大响应（如 GET_VEHICLE_DATA 全量）可能发不出 | 实施期对照 vehicle-command / tesla-ble 源码确认；短期控制响应体积（分域分包）规避 |
| 2 | VCSEC 多响应语义（最多 3 条、终态判定）较绕 | 客户端判定终态出错 | 按 `FromVCSECMessage` 字段规则实现；用 tesla-ble 联调验证 |
| 3 | 域枚举值、剩余 tag 编号未在本档穷举 | 编码不兼容 | **一律以 vendored .proto 为准**，本档不作为字节权威 |
| 4 | tesla-ble 组件版本演进，trust patch 位置变化 | patch 失效 | patch 以最小 diff + Kconfig 隔离；锁定组件版本 |
| 5 | 模拟器时钟与客户端时钟差影响 `expires_at` 校验 | 命令被判过期 | 模拟器对 `expires_at` 从宽校验（或配置项关闭），专注链路验证 |
| 6 | Bless/WinRT 后端 write-with-response 语义与 Bumble 差异 | 两种后端行为不一致 | 以 Bumble (CSR8510) 为加密模式主后端；Bless 做兼容验证 |
| 7 | 附录测试向量的 `TAG_CHALLENGE` 等个别 tag 编号需从 proto 核对 | 单测对不上 | P0 阶段以 protoc 解码示例 hex 反推确认 |

---

## 十一、附录：官方测试密钥与测试向量（P0/P1/P2 单测基准）

来源：`teslamotors/vehicle-command` `pkg/protocol/protocol.md`。**仅用于调试实现，禁止注册到真实车辆。**

### 车辆密钥（模拟器 identity 可固定用它）

```
v  = 0x344EE5B466A7CF1EEB12B6F50331DB2E5EC5834EF5F4BEFCFD8CBE55C2528D70
HEX(ENCODE_PUBLIC(V)) =
04c7a1f47138486aa4729971494878d33b1a24e39571f748a6e16c5955b3d877d
3a6aaa0e955166474af5d32c410f439a2234137ad1bb085fd4e8813c958f11d97
```

### 客户端密钥（单测里扮演 ESP32）

```
c  = 0x2538CDC29A97C19C1E99A637D6CF4F8C970C118B56EDE1E6323E6D162C4B30DB
HEX(ENCODE_PUBLIC(C)) =
04b2b6bc68c2da0665ce656815594996c62394edd8bea905fe781a754fe6a845a7
14330902f225e9269d466e05b349981fda9d85cc23c6fb444aa73b629105dc6e
```

### 密钥协商向量

```
K = 1b2fce19967b79db696f909cff89ea9a
SESSION_INFO_KEY = HMAC-SHA256(K, "session info")
                 = fceb679ee7bca756fcd441bf238bf2f338629b41d9eb9c67be1b32c9672ce300
```

### 握手响应认证向量（VIN = `5YJ30123456789ABC`，请求 uuid = `1588d5a30eabc6f8fc9a951b11f6fd11`）

```
METADATA =
000106021135594a333031323334353637383941424306101588d5a30eabc6f8fc9a951b11f6fd11ff

SESSION_INFO(响应中, hex) =
0806124104c7a1f47138486aa4729971494878d33b1a24e39571f748a6e16c5955b3d877d
3a6aaa0e955166474af5d32c410f439a2234137ad1bb085fd4e8813c958f11d97
1a104c463f9cc0d3d26906e982ed224adde6255a0a0000

期望 tag = HMAC-SHA256(SESSION_INFO_KEY, METADATA || SESSION_INFO)
         = 996c1fe38331be138f8039c194b14db2198846ed7d8251e6749284d7b32ea002
```

### AES-GCM 命令认证向量（"Turn HVAC on"：`CarServer.Action{hvacAutoAction{power_on:true}}` = `120452020801`）

```
METADATA(AAD 原文) =
000105010103021135594a333031323334353637383941424303104c463f9cc0d3d26906e982ed
224adde6040400000a5f050400000007070400000002ff

(该向量 nonce 为随机值，单测时固定 nonce = dbf79447fa156674dae1caed 复现)
nonce     = dbf79447fa156674dae1caed
ciphertext= 38038e8c0f2e
tag       = c228e0ff64991481db3a7bbc133696c5
```

### 完整握手请求示例（protoc 可解码验证）

```
TX hex =
320208023a1212100a7962c10d38b61dd2a7722780a4f0969a031005514f57616bcc81a850
ce0f9d7b482952040a020805
→ UniversalMessage.RoutableMessage：
  to=DOMAIN_VEHICLE_SECURITY, from.routing_address=0a7962...,
  protobuf_message_as_bytes=0a020805 → VCSEC.UnsignedMessage{
    InformationRequest{ INFORMATION_REQUEST_TYPE_GET_WHITELIST_INFO } }
```

---

## 十二、参考文献

1. **官方协议规范（本方案权威来源）**：https://github.com/teslamotors/vehicle-command/blob/main/pkg/protocol/protocol.md
2. 官方 protobuf 定义：https://github.com/teslamotors/vehicle-command/tree/main/protobuf
3. ESP32 客户端组件（路线 A）：https://github.com/yoziru/tesla-ble
4. 本仓既有文档：[HANDOVER.md](HANDOVER.md)（旁路架构）、[SIMULATOR_IMPROVEMENT_PROMPT.md](SIMULATOR_IMPROVEMENT_PROMPT.md)（队列/延迟规划）、[USB_BLUETOOTH_DONGLE_SETUP.md](USB_BLUETOOTH_DONGLE_SETUP.md)（硬件广播配置）
