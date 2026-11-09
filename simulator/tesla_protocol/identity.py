"""车辆身份（NIST P-256）。

模拟器扮演车端，持有车辆私钥 v，并向客户端提供非压缩公钥编码
``0x04 || BE(x, 32) || BE(y, 32)``。

默认使用 Tesla 官方文档中的**调试测试密钥**，便于与 protocol.md 的测试向量
逐字节比对；该私钥本就公开，禁止用于任何真实车辆。
"""

from __future__ import annotations

from cryptography.hazmat.primitives.asymmetric import ec

# 官方 pkg/protocol/protocol.md 测试向量中的车辆私钥（仅调试用）
TEST_VEHICLE_PRIVATE_KEY = 0x344EE5B466A7CF1EEB12B6F50331DB2E5EC5834EF5F4BEFCFD8CBE55C2528D70
TEST_CLIENT_PRIVATE_KEY = 0x2538CDC29A97C19C1E99A637D6CF4F8C970C118B56EDE1E6323E6D162C4B30DB


def encode_public(public_key: ec.EllipticCurvePublicKey) -> bytes:
    """ENCODE_PUBLIC(P) = 0x04 || BIG_ENDIAN(x, 32) || BIG_ENDIAN(y, 32)。"""
    numbers = public_key.public_numbers()
    return (
        b"\x04"
        + numbers.x.to_bytes(32, "big")
        + numbers.y.to_bytes(32, "big")
    )


def decode_public(raw: bytes) -> ec.EllipticCurvePublicKey:
    """解析 65 字节非压缩 P-256 公钥。"""
    if len(raw) != 65 or raw[0] != 0x04:
        raise ValueError(f"非法公钥编码（期望 65 字节 0x04 前缀，实际 {len(raw)} 字节）")
    return ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), raw)


class VehicleIdentity:
    """车端身份：私钥 + 公钥编码 + ECDH。"""

    def __init__(self, private_key_int: int | None = TEST_VEHICLE_PRIVATE_KEY):
        if private_key_int is None:
            self._private = ec.generate_private_key(ec.SECP256R1())
        else:
            self._private = ec.derive_private_key(private_key_int, ec.SECP256R1())
        self.public_key = self._private.public_key()
        self.public_bytes = encode_public(self.public_key)

    @property
    def public_key_hex(self) -> str:
        return self.public_bytes.hex()

    def ecdh(self, peer_public_bytes: bytes) -> bytes:
        """返回共享点 x 坐标（32 字节大端）。"""
        peer = decode_public(peer_public_bytes)
        return self._private.exchange(ec.ECDH(), peer)

    def __repr__(self) -> str:  # pragma: no cover - 调试用
        return f"VehicleIdentity(public={self.public_key_hex[:16]}...)"
