"""VCSEC 配钥（add-key / add-key-request）状态机。

官方 vehicle-command 有两套加钥路径（pkg/vehicle/security.go）：

1. add-key-request（未配对时的引导流程，仅 BLE）
   ```go
   envelope := &vcsec.ToVCSECMessage{
       SignedMessage: &vcsec.SignedMessage{
           ProtobufMessageAsBytes: payload,           // UnsignedMessage{WhitelistOperation{...}}
           SignatureType:          SIGNATURE_TYPE_PRESENT_KEY,
       },
   }
   v.conn.Send(ctx, proto.Marshal(envelope))          // 明文数据报，不经加密会话
   ```
   车辆收到后在中控台提示「轻点 NFC 钥匙卡」，此时回
   CommandStatus{operationStatus = OPERATIONSTATUS_WAIT}；
   用户刷卡 + UI 确认后再回终态 WhitelistOperation_status。
   PRESENT_KEY 不是 ECDSA 签名：`SignedMessage` 里根本没有签名字段，
   它只是告诉 VCSEC「这是钥匙登记请求，需要本地实体（NFC 卡）授权」。

2. add-key（已配对钥匙的常规流程）
   `UnsignedMessage{WhitelistOperation{addKeyToWhitelistAndAddPermissions}}`
   走加密会话，由 dispatcher 处理，不需要刷卡。

本模块只实现第 1 条路径（明文信封 + 刷卡授权），第 2 条在 dispatcher.py。
"""

from __future__ import annotations

import hashlib
import time
from dataclasses import dataclass, field

from .pb import keys_pb2, universal_message_pb2, vcsec_pb2
from .whitelist import Whitelist

# VCSEC.OperationStatus_E
OPERATIONSTATUS_OK = 0
OPERATIONSTATUS_WAIT = 1
OPERATIONSTATUS_ERROR = 2

# VCSEC.WhitelistOperation_information_E
INFO_NONE = 0
INFO_UNDOCUMENTED_ERROR = 1
INFO_WHITELIST_FULL = 4
INFO_NO_PERMISSION_TO_ADD = 5
INFO_INVALID_PUBLIC_KEY = 6
INFO_ALREADY_ON_WHITELIST = 13
INFO_NOT_ALLOWED_TO_ADD_UNLESS_ON_READER = 14
INFO_LOCAL_ENTITY_AUTH_FAILED_UI_DENIED = 24
INFO_LOCAL_ENTITY_AUTH_FAILED_TIMED_OUT_WAITING_FOR_TAP = 25

# UniversalMessage.Domain
DOMAIN_VEHICLE_SECURITY = 2

# VCSEC.KeyFormFactor（缺省按 android 处理，与 ESP32 的 CFG_TESLA_PAIR_FORM_FACTOR 对应）
DEFAULT_FORM_FACTOR = vcsec_pb2.KEY_FORM_FACTOR_ANDROID_DEVICE


@dataclass
class PairingRequest:
    public_key: bytes
    role: int
    form_factor: int
    created_at: float = field(default_factory=time.monotonic)
    wait_sent: bool = False

    def describe(self) -> str:
        role_name = {
            keys_pb2.ROLE_OWNER: "OWNER",
            keys_pb2.ROLE_DRIVER: "DRIVER",
            keys_pb2.ROLE_NONE: "NONE",
        }.get(self.role, str(self.role))
        return f"pub={self.public_key.hex()[:16]}… role={role_name}"


class PairingManager:
    """模拟「车辆等待 NFC 钥匙卡」的配钥授权流程。"""

    def __init__(
        self,
        whitelist: Whitelist,
        logger=None,
        auto_approve: bool = False,
        timeout_s: float = 300.0,
    ):
        self.whitelist = whitelist
        self.log = logger or (lambda msg: None)
        self.auto_approve = auto_approve
        self.timeout_s = float(timeout_s)
        self.pending: PairingRequest | None = None
        self._outbox: list[bytes] = []

    # ── 外部控制（GUI / CLI）────────────────────────────────
    def approve(self) -> bool:
        """模拟「中控台轻点 NFC 钥匙卡 + UI 确认」。返回是否存在待批请求。"""
        req = self.pending
        if req is None:
            return False
        self.pending = None
        entry = self.whitelist.add(req.public_key, req.role, req.form_factor)
        if entry is None:
            info = (
                INFO_ALREADY_ON_WHITELIST
                if self.whitelist.contains(req.public_key)
                else INFO_WHITELIST_FULL
            )
            self.log(f"[PAIR] 授权失败：{req.describe()} info={info}")
            self._outbox.append(self._terminal(info, req.public_key))
            return True
        self.log(f"[PAIR] NFC 卡授权通过：{req.describe()} -> slot={entry.slot}（已可在该域握手）")
        self._outbox.append(self._terminal(INFO_NONE, req.public_key))
        return True

    def reject(self) -> bool:
        req = self.pending
        if req is None:
            return False
        self.pending = None
        self.log(f"[PAIR] 用户拒绝（UI DENIED）：{req.describe()}")
        self._outbox.append(
            self._terminal(INFO_LOCAL_ENTITY_AUTH_FAILED_UI_DENIED, req.public_key)
        )
        return True

    def drain(self) -> list[bytes]:
        out, self._outbox = self._outbox, []
        return out

    def tick(self, now: float | None = None) -> list[bytes]:
        """超时未刷卡 -> 回 TIMED_OUT_WAITING_FOR_TAP 终态。"""
        now = time.monotonic() if now is None else now
        req = self.pending
        if req is None:
            return []
        if now - req.created_at < self.timeout_s:
            return []
        self.pending = None
        self.log(f"[PAIR] 等待 NFC 卡超时（{self.timeout_s:.0f}s）：{req.describe()}")
        self._outbox.append(
            self._terminal(INFO_LOCAL_ENTITY_AUTH_FAILED_TIMED_OUT_WAITING_FOR_TAP, req.public_key)
        )
        return self.drain()

    # ── 明文信封入口 ────────────────────────────────────────
    def handle_envelope(self, envelope, now: float | None = None) -> list[bytes]:
        """处理 ToVCSECMessage 明文数据报，返回待回包（RoutableMessage 字节）。"""
        now = time.monotonic() if now is None else now

        signed = envelope.signedMessage
        if signed.signatureType != vcsec_pb2.SIGNATURE_TYPE_PRESENT_KEY:
            self.log(
                "[PAIR] 信封签名类型非法 "
                f"{signed.signatureType}（期望 SIGNATURE_TYPE_PRESENT_KEY=2）"
            )
            return [self._terminal(INFO_UNDOCUMENTED_ERROR, b"")]

        msg = vcsec_pb2.UnsignedMessage()
        try:
            msg.ParseFromString(signed.protobufMessageAsBytes)
        except Exception as exc:
            self.log(f"[PAIR] UnsignedMessage 解析失败: {exc}")
            return [self._terminal(INFO_UNDOCUMENTED_ERROR, b"")]

        if msg.WhichOneof("sub_message") != "WhitelistOperation":
            self.log(f"[PAIR] 明文信封只允许 WhitelistOperation，收到 {msg.WhichOneof('sub_message')}")
            return [self._terminal(INFO_UNDOCUMENTED_ERROR, b"")]

        op = msg.WhitelistOperation
        sub = op.WhichOneof("sub_message")

        if sub == "addKeyToWhitelistAndAddPermissions":
            change = op.addKeyToWhitelistAndAddPermissions
            public_key = bytes(change.key.PublicKeyRaw)
            role = int(change.keyRole) if change.keyRole else keys_pb2.ROLE_OWNER
        elif sub == "addPublicKeyToWhitelist":
            public_key = bytes(op.addPublicKeyToWhitelist.PublicKeyRaw)
            role = keys_pb2.ROLE_NONE
        else:
            self.log(f"[PAIR] 未授权请求不能执行白名单操作: {sub}")
            return [self._terminal(INFO_NOT_ALLOWED_TO_ADD_UNLESS_ON_READER, b"")]

        if not public_key or len(public_key) != 65 or public_key[0] != 0x04:
            self.log(f"[PAIR] 公钥编码非法：{len(public_key)}B")
            return [self._terminal(INFO_INVALID_PUBLIC_KEY, public_key)]

        if self.whitelist.contains(public_key):
            self.log(f"[PAIR] 该公钥已在白名单：{public_key.hex()[:16]}…")
            return [self._terminal(INFO_ALREADY_ON_WHITELIST, public_key)]

        if self.whitelist.is_full():
            self.log("[PAIR] 白名单已满（KEYCHAIN / WHITELIST_FULL）")
            return [self._terminal(INFO_WHITELIST_FULL, public_key)]

        form_factor = int(op.metadataForKey.keyFormFactor) or DEFAULT_FORM_FACTOR
        req = PairingRequest(
            public_key=public_key, role=role, form_factor=form_factor, created_at=now
        )

        if self.auto_approve:
            self.log(f"[PAIR] 收到配钥请求（自动批准模式）：{req.describe()}")
            self.pending = req
            self.approve()
            return self.drain()

        self.pending = req
        req.wait_sent = True
        self.log(
            f"[PAIR] 收到配钥请求：{req.describe()} —— 等待 NFC 钥匙卡"
            "（模拟器界面点击「🔑 轻点钥匙卡」，或 CLI 加 --pair-auto）"
        )
        return [self._wait_reply()]

    # ── 回包构造 ────────────────────────────────────────────
    @staticmethod
    def _wrap(from_vcsec) -> bytes:
        reply = universal_message_pb2.RoutableMessage()
        reply.from_destination.domain = DOMAIN_VEHICLE_SECURITY
        reply.protobuf_message_as_bytes = from_vcsec.SerializeToString()
        return reply.SerializeToString()

    def _wait_reply(self) -> bytes:
        reply = vcsec_pb2.FromVCSECMessage()
        reply.commandStatus.operationStatus = OPERATIONSTATUS_WAIT
        return self._wrap(reply)

    def _terminal(self, info: int, public_key: bytes) -> bytes:
        reply = vcsec_pb2.FromVCSECMessage()
        status = reply.commandStatus
        status.operationStatus = (
            OPERATIONSTATUS_OK if info == INFO_NONE else OPERATIONSTATUS_ERROR
        )
        wl = status.whitelistOperationStatus
        wl.whitelistOperationInformation = info
        wl.operationStatus = (
            OPERATIONSTATUS_OK if info == INFO_NONE else OPERATIONSTATUS_ERROR
        )
        if public_key:
            # KeyIdentifier.publicKeySHA1 = SHA1(公钥原始字节)
            wl.signerOfOperation.publicKeySHA1 = hashlib.sha1(public_key).digest()
        return self._wrap(reply)
