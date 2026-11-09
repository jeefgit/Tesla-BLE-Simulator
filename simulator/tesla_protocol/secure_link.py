"""安全链路层：帧封装 / 流式重组 / HMAC / AES-128-GCM。"""

from __future__ import annotations

import hashlib
import hmac
import os
import struct
import time

from cryptography.exceptions import InvalidTag
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

from .metadata import SIG_TYPE_AES_GCM_PERSONALIZED, SIG_TYPE_HMAC_PERSONALIZED
from .session import DOMAIN_VEHICLE_SECURITY

MAX_FRAME_BYTES = 4096


def frame(payload: bytes) -> bytes:
    """BLE 传输帧 = 2 字节大端长度 + protobuf 字节。"""
    if len(payload) > 0xFFFF:
        raise ValueError(f"帧过大: {len(payload)}")
    return struct.pack(">H", len(payload)) + payload


def unframe(data: bytes) -> bytes:
    if len(data) < 2:
        raise ValueError("帧长度不足 2 字节")
    declared = struct.unpack(">H", data[:2])[0]
    if declared + 2 != len(data):
        raise ValueError(f"帧长度不自洽: 声明 {declared}，实际 {len(data) - 2}")
    return data[2:]


class FrameReassembler:
    """把 BLE 多次 write 的字节流重组成完整帧。

    客户端按 MTU-3 分块写入，因此一次 write 可能只是半帧；超时（默认 2s）
    未凑满则丢弃缓冲，避免半包悬挂。
    """

    def __init__(self, max_bytes: int = MAX_FRAME_BYTES, timeout_s: float = 2.0):
        self._buf = bytearray()
        self._max_bytes = max_bytes
        self._timeout_s = timeout_s
        self._last_feed = 0.0

    def reset(self) -> None:
        self._buf.clear()
        self._last_feed = 0.0

    def feed(self, chunk: bytes) -> list[bytes]:
        """喂入字节，返回本次凑齐的所有完整帧（已去掉长度前缀）。"""
        now = time.time()
        if self._buf and now - self._last_feed > self._timeout_s:
            self._buf.clear()
        self._last_feed = now
        self._buf.extend(chunk)

        frames: list[bytes] = []
        while len(self._buf) >= 2:
            declared = struct.unpack(">H", self._buf[:2])[0]
            if declared == 0 or declared > self._max_bytes:
                # 非法长度：丢弃首字节重新同步
                del self._buf[0]
                continue
            if len(self._buf) < declared + 2:
                break
            frames.append(bytes(self._buf[2:2 + declared]))
            del self._buf[: declared + 2]
        return frames

    def pending(self) -> int:
        return len(self._buf)


def hmac_sha256(key: bytes, data: bytes) -> bytes:
    return hmac.new(key, data, hashlib.sha256).digest()


def sha256(data: bytes) -> bytes:
    return hashlib.sha256(data).digest()


def constant_time_equals(a: bytes, b: bytes) -> bool:
    return hmac.compare_digest(a, b)


def encrypt_gcm(key: bytes, plaintext: bytes, aad: bytes) -> tuple[bytes, bytes, bytes]:
    """AES-128-GCM 加密，返回 (nonce, ciphertext, tag)。"""
    nonce = os.urandom(12)
    sealed = AESGCM(key).encrypt(nonce, plaintext, aad)
    return nonce, sealed[:-16], sealed[-16:]


def decrypt_gcm_with_aad(
    key: bytes,
    nonce: bytes,
    ciphertext: bytes,
    tag: bytes,
    aad_candidates: list[bytes],
) -> tuple[bytes, bytes] | tuple[None, None]:
    """依次尝试候选 AAD 解密，返回 (plaintext, matched_aad) 或 (None, None)。"""
    for aad in aad_candidates:
        try:
            plaintext = AESGCM(key).decrypt(nonce, ciphertext + tag, aad)
            return plaintext, aad
        except (InvalidTag, ValueError):
            continue
    return None, None


def build_request_hash(sig_type: int, auth_tag: bytes, domain: int) -> bytes:
    """请求哈希 = 1 字节认证方式 + 请求认证 tag（VCSEC 截断到 17 字节）。"""
    digest = bytes([sig_type]) + auth_tag
    if domain == DOMAIN_VEHICLE_SECURITY:
        return digest[:17]
    return digest


SIGNATURE_TYPE_AES_GCM_PERSONALIZED = SIG_TYPE_AES_GCM_PERSONALIZED
SIGNATURE_TYPE_HMAC_PERSONALIZED = SIG_TYPE_HMAC_PERSONALIZED
