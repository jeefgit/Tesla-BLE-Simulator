"""特斯拉车端加密协议服务器（模拟器核心）。

把 identity / metadata / session / secure_link / dispatcher 串成完整的
握手 + 命令认证 + 响应加密状态机，直接对接 BLE 的 on_write 字节流。

支持的认证方式（与官方 vehicle-command 一致）：
- AES-128-GCM Personalized：加密命令 + 可选加密响应；
- HMAC-SHA256 Personalized：明文命令 + HMAC 校验。
"""

from __future__ import annotations

import secrets
import time

from .dispatcher import DispatchResult, Dispatcher
from .identity import VehicleIdentity
from .metadata import (
    SIG_TYPE_AES_GCM_PERSONALIZED,
    SIG_TYPE_HMAC_PERSONALIZED,
    checksum,
    request_metadata_candidates,
    response_metadata,
    session_info_metadata,
)
from .pb import signatures_pb2, universal_message_pb2
from .secure_link import (
    FrameReassembler,
    build_request_hash,
    constant_time_equals,
    decrypt_gcm_with_aad,
    encrypt_gcm,
    frame,
    hmac_sha256,
)
from .session import (
    DOMAIN_INFOTAINMENT,
    DOMAIN_VEHICLE_SECURITY,
    DomainSession,
    derive_command_key,
    derive_session_info_key,
    derive_shared_key,
    new_epoch,
)

# UniversalMessage.MessageFault_E
FAULT_NONE = 0
FAULT_INVALID_SIGNATURE = 5
FAULT_INVALID_TOKEN_OR_COUNTER = 6
FAULT_INVALID_DOMAINS = 8
FAULT_INVALID_COMMAND = 9
FAULT_DECODING = 10
FAULT_INCORRECT_EPOCH = 15
FAULT_TIME_EXPIRED = 17

# UniversalMessage.Flags（FLAG_ENCRYPT_RESPONSE 的位号是 1）
FLAG_ENCRYPT_RESPONSE_BIT = 1
FLAG_ENCRYPT_RESPONSE = 1 << FLAG_ENCRYPT_RESPONSE_BIT

# UniversalMessage.OperationStatus_E
OPERATIONSTATUS_OK = 0
OPERATIONSTATUS_ERROR = 2

SUPPORTED_DOMAINS = (DOMAIN_VEHICLE_SECURITY, DOMAIN_INFOTAINMENT)

# 命令过期容忍：车端时钟与客户端时钟存在秒级偏差
EXPIRY_TOLERANCE_S = 15


class TeslaCryptoServer:
    """车端加密协议状态机。"""

    def __init__(
        self,
        vin: str,
        telemetry=None,
        logger=None,
        identity: VehicleIdentity | None = None,
        allowed_client_public: bytes | None = None,
    ):
        self.vin = vin
        self.identity = identity or VehicleIdentity()
        self.dispatcher = Dispatcher(vin, telemetry, logger)
        self.log = logger or (lambda msg: None)
        self.sessions: dict[int, DomainSession] = {}
        self.reassembler = FrameReassembler()
        # 模拟 2024.38 之前的老固件：忽略 FLAG_ENCRYPT_RESPONSE，响应保持明文
        self.response_encryption_enabled = True
        # 车端配对白名单：非 None 时只接受该客户端公钥（65B 非压缩编码），
        # 模拟真车「未登记公钥的客户端一律拒绝握手」的行为；None 表示不校验。
        self.allowed_client_public = allowed_client_public

    # ── 生命周期 ────────────────────────────────────────────
    def reset(self) -> None:
        self.sessions.clear()
        self.reassembler.reset()

    def set_vin(self, vin: str) -> None:
        if vin != self.vin:
            self.vin = vin
            self.dispatcher.vin = vin
            self.reset()

    def describe(self) -> list[dict]:
        return [s.summary() for s in self.sessions.values()]

    # ── BLE 入口 ────────────────────────────────────────────
    def feed(self, chunk: bytes) -> list[bytes]:
        """喂入一次 BLE write 的原始字节，返回待 notify 的完整帧列表。"""
        if not chunk:
            return []
        responses: list[bytes] = []
        for payload in self.reassembler.feed(chunk):
            try:
                reply = self.handle_message(payload)
            except Exception as exc:  # pragma: no cover - 防御性
                self.log(f"[CRYPTO] 消息处理异常: {exc}")
                reply = None
            if reply:
                responses.append(frame(reply))
        return responses

    def handle_message(self, payload: bytes) -> bytes | None:
        """处理一个已去掉长度前缀的 RoutableMessage。"""
        msg = universal_message_pb2.RoutableMessage()
        try:
            msg.ParseFromString(payload)
        except Exception as exc:
            self.log(f"[CRYPTO] RoutableMessage 解析失败: {exc}")
            return None

        if msg.HasField("session_info_request"):
            return self._handle_handshake(msg)
        if msg.HasField("protobuf_message_as_bytes"):
            return self._handle_command(msg)
        self.log("[CRYPTO] 未知 payload 类型")
        return self._build_fault(msg, None, FAULT_INVALID_COMMAND)

    # ── 握手 ──────────────────────────────────────────────
    def _handle_handshake(self, msg) -> bytes:
        domain = self._domain(msg)
        client_public = bytes(msg.session_info_request.public_key)
        if domain not in SUPPORTED_DOMAINS:
            self.log(f"[CRYPTO] 握手目标域非法: {domain}")
            return self._build_fault(msg, domain, FAULT_INVALID_DOMAINS)
        if len(client_public) != 65 or client_public[0] != 0x04:
            self.log(f"[CRYPTO] 客户端公钥编码非法: {len(client_public)}B")
            return self._build_fault(msg, domain, FAULT_INVALID_SIGNATURE)
        if self.allowed_client_public is not None and not constant_time_equals(
            client_public, self.allowed_client_public
        ):
            self.log(
                f"[CRYPTO] 客户端公钥未登记，拒绝握手 pub={client_public.hex()[:16]}…"
                f"（车端仅接受 {self.allowed_client_public.hex()[:16]}…）"
            )
            return self._build_fault(msg, domain, FAULT_INVALID_SIGNATURE)

        try:
            shared_x = self.identity.ecdh(client_public)
        except Exception as exc:
            self.log(f"[CRYPTO] ECDH 失败: {exc}")
            return self._build_fault(msg, domain, FAULT_INVALID_SIGNATURE)

        shared_key = derive_shared_key(shared_x)
        session = DomainSession(
            domain=domain,
            client_public=client_public,
            shared_key=shared_key,
            session_info_key=derive_session_info_key(shared_key),
            command_key=derive_command_key(shared_key),
            epoch=new_epoch(),
            clock_offset=0,
            initial_counter=self._new_counter(),
            routing_address=self._routing_of(msg),
        )
        self.sessions[domain] = session

        challenge = self._challenge_of(msg)
        session_info = signatures_pb2.SessionInfo()
        session_info.counter = session.initial_counter
        session_info.publicKey = self.identity.public_bytes
        session_info.epoch = session.epoch
        session_info.clock_time = session.domain_now()
        session_info.status = signatures_pb2.SESSION_INFO_STATUS_OK
        session_info_bytes = session_info.SerializeToString()

        metadata = session_info_metadata(self.vin, challenge)
        tag = hmac_sha256(session.session_info_key, metadata + session_info_bytes)

        reply = universal_message_pb2.RoutableMessage()
        reply.to_destination.routing_address = session.routing_address
        reply.from_destination.domain = domain
        reply.session_info = session_info_bytes
        reply.signature_data.session_info_tag.tag = tag
        if challenge:
            reply.request_uuid = challenge

        self.log(
            f"[CRYPTO] 握手成功 域={self._domain_name(domain)} "
            f"counter={session.initial_counter} epoch={session.epoch.hex()[:8]} "
            f"key={shared_key.hex()[:8]}"
        )
        return reply.SerializeToString()

    # ── 命令 ──────────────────────────────────────────────
    def _handle_command(self, msg) -> bytes:
        domain = self._domain(msg)
        session = self.sessions.get(domain)
        if session is None:
            self.log(f"[CRYPTO] 命令来自未握手域: {domain}")
            return self._build_fault(msg, domain, FAULT_INVALID_DOMAINS)

        sig = msg.signature_data
        sig_kind = sig.WhichOneof("sig_type")

        if sig_kind == "AES_GCM_Personalized_data":
            sig_type = SIG_TYPE_AES_GCM_PERSONALIZED
            sd = sig.AES_GCM_Personalized_data
            nonce = bytes(sd.nonce)
            auth_tag = bytes(sd.tag)
            ciphertext = bytes(msg.protobuf_message_as_bytes)
        elif sig_kind == "HMAC_Personalized_data":
            sig_type = SIG_TYPE_HMAC_PERSONALIZED
            sd = sig.HMAC_Personalized_data
            nonce = b""
            auth_tag = bytes(sd.tag)
            ciphertext = bytes(msg.protobuf_message_as_bytes)
        else:
            self.log(f"[CRYPTO] 缺少签名数据: {sig_kind}")
            return self._build_fault(msg, domain, FAULT_INVALID_SIGNATURE)

        counter = int(sd.counter)
        epoch = bytes(sd.epoch)
        expires_at = int(sd.expires_at)
        flags = int(msg.flags)

        if epoch and epoch != session.epoch:
            self.log("[CRYPTO] epoch 不匹配，要求客户端重新握手")
            return self._build_fault(msg, domain, FAULT_INCORRECT_EPOCH)
        if not session.accepts_counter(counter):
            self.log(f"[CRYPTO] 重放/乱序计数器被拒绝: {counter}")
            return self._build_fault(msg, domain, FAULT_INVALID_TOKEN_OR_COUNTER)
        if expires_at and expires_at < session.domain_now() - EXPIRY_TOLERANCE_S:
            self.log(f"[CRYPTO] 命令已过期: {expires_at} < {session.domain_now()}")
            return self._build_fault(msg, domain, FAULT_TIME_EXPIRED)

        aad_candidates = [
            checksum(meta)
            for meta in request_metadata_candidates(
                domain, self.vin, epoch, counter, expires_at, flags, sig_type
            )
        ]

        if sig_type == SIG_TYPE_AES_GCM_PERSONALIZED:
            plaintext, _ = decrypt_gcm_with_aad(
                session.shared_key, nonce, ciphertext, auth_tag, aad_candidates
            )
            if plaintext is None:
                self.log("[CRYPTO] AES-GCM 解密/认证失败")
                return self._build_fault(msg, domain, FAULT_INVALID_SIGNATURE)
        else:
            plaintext = ciphertext
            verified = False
            for aad in aad_candidates:
                expected = hmac_sha256(session.command_key, aad + ciphertext)
                if constant_time_equals(expected, auth_tag):
                    verified = True
                    break
            if not verified:
                self.log("[CRYPTO] HMAC 校验失败")
                return self._build_fault(msg, domain, FAULT_INVALID_SIGNATURE)

        session.accept_counter(counter)
        result: DispatchResult = self.dispatcher.handle(domain, plaintext, session.client_public)
        return self._build_response(msg, session, domain, result, sig_type, auth_tag, flags)

    # ── 响应构造 ──────────────────────────────────────────
    def _build_response(
        self,
        request,
        session: DomainSession,
        domain: int,
        result: DispatchResult,
        req_sig_type: int,
        req_tag: bytes,
        req_flags: int,
    ) -> bytes:
        reply = universal_message_pb2.RoutableMessage()
        reply.to_destination.routing_address = self._routing_of(request)
        reply.from_destination.domain = domain
        reply.flags = req_flags
        uuid = self._challenge_of(request)
        if uuid:
            reply.request_uuid = uuid

        fault = int(result.fault)
        reply.signedMessageStatus.operation_status = (
            OPERATIONSTATUS_OK if fault == FAULT_NONE else OPERATIONSTATUS_ERROR
        )
        reply.signedMessageStatus.signed_message_fault = fault

        plaintext = result.plaintext or b""
        if (req_flags & FLAG_ENCRYPT_RESPONSE) and self.response_encryption_enabled:
            counter = session.next_response_counter()
            request_hash = build_request_hash(req_sig_type, req_tag, domain)
            metadata = response_metadata(
                domain, self.vin, counter, req_flags, request_hash, fault
            )
            nonce, ciphertext, tag = encrypt_gcm(
                session.shared_key, plaintext, checksum(metadata)
            )
            reply.protobuf_message_as_bytes = ciphertext
            reply.signature_data.AES_GCM_Response_data.nonce = nonce
            reply.signature_data.AES_GCM_Response_data.counter = counter
            reply.signature_data.AES_GCM_Response_data.tag = tag
        else:
            reply.protobuf_message_as_bytes = plaintext

        return reply.SerializeToString()

    def _build_fault(self, request, domain: int | None, fault: int) -> bytes:
        """协议层错误响应：明文负载 + signedMessageStatus。"""
        reply = universal_message_pb2.RoutableMessage()
        routing = self._routing_of(request)
        if routing:
            reply.to_destination.routing_address = routing
        if domain in SUPPORTED_DOMAINS:
            reply.from_destination.domain = domain
        uuid = self._challenge_of(request)
        if uuid:
            reply.request_uuid = uuid
        reply.signedMessageStatus.operation_status = OPERATIONSTATUS_ERROR
        reply.signedMessageStatus.signed_message_fault = fault
        return reply.SerializeToString()

    # ── 辅助 ──────────────────────────────────────────────
    @staticmethod
    def _new_counter() -> int:
        return secrets.randbelow(1 << 24) + 1

    @staticmethod
    def _routing_of(msg) -> bytes:
        if msg.HasField("from_destination") and msg.from_destination.HasField("routing_address"):
            return bytes(msg.from_destination.routing_address)
        return b""

    @staticmethod
    def _challenge_of(msg) -> bytes:
        if msg.uuid:
            return bytes(msg.uuid)
        if msg.HasField("session_info_request") and msg.session_info_request.challenge:
            return bytes(msg.session_info_request.challenge)
        return bytes(msg.request_uuid)

    def _domain(self, msg) -> int | None:
        if msg.HasField("to_destination"):
            dest = msg.to_destination
            if dest.HasField("domain"):
                return int(dest.domain)
            if dest.HasField("routing_address"):
                addr = bytes(dest.routing_address)
                for domain, session in self.sessions.items():
                    if session.routing_address == addr:
                        return domain
        return None

    @staticmethod
    def _domain_name(domain: int | None) -> str:
        return {
            DOMAIN_VEHICLE_SECURITY: "VCSEC",
            DOMAIN_INFOTAINMENT: "Infotainment",
        }.get(domain, f"Domain({domain})")
