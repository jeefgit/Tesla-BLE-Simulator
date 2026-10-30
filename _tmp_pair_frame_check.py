"""对比固件手写 add-key-request 编码 与 官方 proto(pb2) 生成的字节。

固件 TeslaCrypto::buildPairingRequest() 的编码顺序/字段号在这里逐字段复刻，
与 tesla_protocol/pb 里由官方 .proto 生成的 pb2 输出做逐字节比对。
两者完全一致才说明真车收到的是标准信封。
"""

from __future__ import annotations

import sys

from tesla_protocol.pb import keys_pb2, vcsec_pb2

PUB_HEX = "048fa445f6d89310a185bad86fc22aba133ea68541cdc638f50a674a702d6761cc659f5dfe12125cae48545102080464a60c806244f0cc06a7f865a869625f008c"
ROLE = keys_pb2.ROLE_OWNER            # 2
FORM_FACTOR = vcsec_pb2.KEY_FORM_FACTOR_CLOUD_KEY  # 9
SIG_PRESENT_KEY = vcsec_pb2.SIGNATURE_TYPE_PRESENT_KEY  # 2


def official_bytes() -> bytes:
    """官方 vehicle-command pkg/vehicle/security.go::SendAddKeyRequestWithRole 等价编码。"""
    payload = vcsec_pb2.UnsignedMessage()
    change = payload.WhitelistOperation.addKeyToWhitelistAndAddPermissions
    change.key.PublicKeyRaw = bytes.fromhex(PUB_HEX)
    change.keyRole = ROLE
    payload.WhitelistOperation.metadataForKey.keyFormFactor = FORM_FACTOR

    envelope = vcsec_pb2.ToVCSECMessage()
    envelope.signedMessage.protobufMessageAsBytes = payload.SerializeToString()
    envelope.signedMessage.signatureType = SIG_PRESENT_KEY
    return envelope.SerializeToString()


# ---- 固件 pb::Writer 复刻（只实现用到的方法） ----
class Writer:
    def __init__(self):
        self.b = bytearray()

    def tag(self, field, wire):
        v = (field << 3) | wire
        while v >= 0x80:
            self.b.append((v & 0x7F) | 0x80)
            v >>= 7
        self.b.append(v)

    def varint(self, v):
        while v >= 0x80:
            self.b.append((v & 0x7F) | 0x80)
            v >>= 7
        self.b.append(v)

    def uint32(self, field, v):
        self.tag(field, 0)
        self.varint(v)

    def bytes_(self, field, data):
        self.tag(field, 2)
        self.varint(len(data))
        self.b.extend(data)

    def message(self, field, sub):
        self.bytes_(field, bytes(sub.b))


def firmware_bytes(pub: bytes) -> bytes:
    """复刻 TeslaCrypto.cpp:609-656 的字段顺序。"""
    key = Writer()
    key.bytes_(1, pub)                       # PublicKey.publicKeyRaw = 1

    change = Writer()
    change.message(1, key)                   # PermissionChange.key = 1
    change.uint32(4, ROLE)                   # PermissionChange.keyRole = 4

    meta = Writer()
    meta.uint32(1, FORM_FACTOR)              # KeyMetadata.keyFormFactor = 1

    op = Writer()
    op.message(5, change)                    # addKeyToWhitelistAndAddPermissions = 5
    op.message(6, meta)                      # metadataForKey = 6

    um = Writer()
    um.message(16, op)                       # UnsignedMessage.WhitelistOperation = 16

    sm = Writer()
    sm.bytes_(2, bytes(um.b))                # SignedMessage.protobufMessageAsBytes = 2
    sm.uint32(3, SIG_PRESENT_KEY)            # SignedMessage.signatureType = 3

    w = Writer()
    w.message(1, sm)                         # ToVCSECMessage.signedMessage = 1
    return bytes(w.b)


def main() -> int:
    pub = bytes.fromhex(PUB_HEX)
    assert len(pub) == 65 and pub[0] == 0x04, "公钥必须是 65 字节 0x04 开头"

    official = official_bytes()
    fw = firmware_bytes(pub)
    wire_total = 2 + len(fw)  # 2 字节大端长度前缀后的总字节数

    print(f"官方 pb2 编码 : {len(official)}B")
    print(f"固件手写编码 : {len(fw)}B")
    print(f"带长度前缀后 : {wire_total}B")
    print(f"长度前缀     : {len(fw):04x}")
    print(f"官方 hex     : {official.hex()}")
    print(f"固件 hex     : {fw.hex()}")

    if official == fw:
        print("RESULT: IDENTICAL —— 信封与官方 vehicle-command 完全一致")
        return 0

    print("RESULT: MISMATCH")
    for i, (a, b) in enumerate(zip(official, fw)):
        if a != b:
            print(f"  首个差异 offset={i}: official={a:02x} firmware={b:02x}")
            break
    return 1


if __name__ == "__main__":
    sys.exit(main())
