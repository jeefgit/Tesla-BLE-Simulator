"""元数据 TLV 序列化（协议认证绑定的核心）。

规则（见官方 ``internal/authentication/metadata.go`` 与 protocol.md）：
- 每个键值对编码为 ``TAG || LEN || VALUE``；
- ``TAG_SIGNATURE_TYPE`` / ``TAG_DOMAIN`` 的值为 **1 字节**；
- 计数器 / 过期时间 / 标志为 **4 字节大端**；
- 按 TAG 升序拼接，末尾追加 ``0xFF``；
- AEAD 的 AAD = ``SHA256(TLV串)``（官方 ``metadata.Checksum(nil)``）。
"""

from __future__ import annotations

import hashlib
from typing import Iterable

# Signatures.Tag
TAG_SIGNATURE_TYPE = 0
TAG_DOMAIN = 1
TAG_PERSONALIZATION = 2
TAG_EPOCH = 3
TAG_EXPIRES_AT = 4
TAG_COUNTER = 5
TAG_CHALLENGE = 6
TAG_FLAGS = 7
TAG_REQUEST_HASH = 8
TAG_FAULT = 9
TAG_END = 0xFF

# Signatures.SignatureType
SIG_TYPE_AES_GCM = 0
SIG_TYPE_AES_GCM_PERSONALIZED = 5
SIG_TYPE_HMAC = 6
SIG_TYPE_HMAC_PERSONALIZED = 8
SIG_TYPE_AES_GCM_RESPONSE = 9


def u32(value: int) -> bytes:
    """uint32 大端编码（TLV 中整数统一 4 字节）。"""
    return int(value).to_bytes(4, "big")


def _tlv(tag: int, value: bytes) -> bytes:
    if len(value) > 255:
        raise ValueError(f"TLV 值过长: tag={tag} len={len(value)}")
    return bytes([tag, len(value)]) + value


def serialize(items: Iterable[tuple[int, bytes]]) -> bytes:
    """按 tag 升序序列化并追加 0xFF 结束符。"""
    encoded = sorted(
        ((int(tag), bytes(value)) for tag, value in items), key=lambda kv: kv[0]
    )
    body = b"".join(_tlv(tag, value) for tag, value in encoded)
    return body + bytes([TAG_END])


def checksum(serialized: bytes) -> bytes:
    """AEAD AAD = SHA256(serialized)。"""
    return hashlib.sha256(serialized).digest()


def session_info_metadata(vin: str, challenge: bytes) -> bytes:
    """握手响应的元数据：HMAC + VIN + 请求 uuid。"""
    return serialize(
        [
            (TAG_SIGNATURE_TYPE, bytes([SIG_TYPE_HMAC])),
            (TAG_PERSONALIZATION, vin.encode("utf-8")),
            (TAG_CHALLENGE, challenge),
        ]
    )


def request_metadata(
    domain: int,
    vin: str,
    epoch: bytes,
    counter: int,
    expires_at: int,
    flags: int,
    sig_type: int = SIG_TYPE_AES_GCM_PERSONALIZED,
) -> bytes:
    """命令请求元数据（作为 AES-GCM 的 AAD 原文）。

    ``TAG_FLAGS`` 仅在非零时包含（与官方客户端一致）。
    """
    items: list[tuple[int, bytes]] = [
        (TAG_SIGNATURE_TYPE, bytes([sig_type])),
        (TAG_DOMAIN, bytes([domain])),
        (TAG_PERSONALIZATION, vin.encode("utf-8")),
    ]
    if epoch:
        items.append((TAG_EPOCH, epoch))
    if expires_at:
        items.append((TAG_EXPIRES_AT, u32(expires_at)))
    items.append((TAG_COUNTER, u32(counter)))
    if flags:
        items.append((TAG_FLAGS, u32(flags)))
    return serialize(items)


def request_metadata_candidates(
    domain: int,
    vin: str,
    epoch: bytes,
    counter: int,
    expires_at: int,
    flags: int,
    sig_type: int = SIG_TYPE_AES_GCM_PERSONALIZED,
) -> list[bytes]:
    """请求元数据的若干合法变体（提高与不同客户端实现的互操作性）。

    不同客户端对「epoch / expires_at / flags 缺省时是否写入 TLV」处理不一，
    模拟器依次尝试这些候选，用 AEAD/HMAC 校验结果反推正确的那一份。
    """
    variants: list[bytes] = [
        request_metadata(domain, vin, epoch, counter, expires_at, flags, sig_type)
    ]
    if epoch:
        variants.append(
            request_metadata(domain, vin, b"", counter, expires_at, flags, sig_type)
        )
    if expires_at:
        variants.append(
            request_metadata(domain, vin, epoch, counter, 0, flags, sig_type)
        )
    if flags:
        variants.append(
            request_metadata(domain, vin, epoch, counter, expires_at, 0, sig_type)
        )
    # 去重且保持顺序
    unique: list[bytes] = []
    for item in variants:
        if item not in unique:
            unique.append(item)
    return unique


def response_metadata(
    domain: int,
    vin: str,
    counter: int,
    flags: int,
    request_hash: bytes,
    fault: int = 0,
) -> bytes:
    """响应元数据（作为响应密文的 AAD 原文）。

    官方 ``Peer.responseMetadata`` 总是写入 TAG_FLAGS 与 TAG_FAULT（即使为 0）。
    """
    return serialize(
        [
            (TAG_SIGNATURE_TYPE, bytes([SIG_TYPE_AES_GCM_RESPONSE])),
            (TAG_DOMAIN, bytes([domain])),
            (TAG_PERSONALIZATION, vin.encode("utf-8")),
            (TAG_COUNTER, u32(counter)),
            (TAG_FLAGS, u32(flags)),
            (TAG_REQUEST_HASH, request_hash),
            (TAG_FAULT, u32(fault)),
        ]
    )


def response_metadata_candidates(
    domain: int,
    vin: str,
    counter: int,
    flags: int,
    request_hash: bytes,
    fault: int = 0,
) -> list[bytes]:
    """响应元数据变体（部分实现可能省略 TAG_FAULT=0）。"""
    variants = [response_metadata(domain, vin, counter, flags, request_hash, fault)]
    if not fault:
        variants.append(
            serialize(
                [
                    (TAG_SIGNATURE_TYPE, bytes([SIG_TYPE_AES_GCM_RESPONSE])),
                    (TAG_DOMAIN, bytes([domain])),
                    (TAG_PERSONALIZATION, vin.encode("utf-8")),
                    (TAG_COUNTER, u32(counter)),
                    (TAG_FLAGS, u32(flags)),
                    (TAG_REQUEST_HASH, request_hash),
                ]
            )
        )
    unique: list[bytes] = []
    for item in variants:
        if item not in unique:
            unique.append(item)
    return unique
