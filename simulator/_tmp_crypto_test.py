import os
import hashlib
import secrets
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

from tesla_protocol.identity import (
    TEST_CLIENT_PRIVATE_KEY,
    TEST_VEHICLE_PRIVATE_KEY,
    VehicleIdentity,
    encode_public,
)
from tesla_protocol.metadata import (
    SIG_TYPE_AES_GCM_PERSONALIZED,
    SIG_TYPE_HMAC,
    checksum,
    request_metadata,
    response_metadata,
    session_info_metadata,
)
from tesla_protocol.pb import car_server_pb2, keys_pb2, signatures_pb2, universal_message_pb2, vcsec_pb2
from tesla_protocol.pairing import (
    INFO_NONE,
    INFO_NO_PERMISSION_TO_ADD,
    OPERATIONSTATUS_OK,
    OPERATIONSTATUS_WAIT,
)
from tesla_protocol.secure_link import build_request_hash, frame, hmac_sha256, unframe
from tesla_protocol.server import FLAG_ENCRYPT_RESPONSE, TeslaCryptoServer
from tesla_protocol.session import (
    DOMAIN_INFOTAINMENT,
    DOMAIN_VEHICLE_SECURITY,
    derive_command_key,
    derive_session_info_key,
    derive_shared_key,
)

VIN = "5YJ30123456789ABC"


def client_pub_bytes(priv_int):
    priv = ec.derive_private_key(priv_int, ec.SECP256R1())
    return encode_public(priv.public_key())


def test_official_ecdh():
    vehicle = VehicleIdentity(TEST_VEHICLE_PRIVATE_KEY)
    client_priv = ec.derive_private_key(TEST_CLIENT_PRIVATE_KEY, ec.SECP256R1())
    shared_x = client_priv.exchange(ec.ECDH(), vehicle.public_key)
    k = derive_shared_key(shared_x)
    assert k.hex() == "1b2fce19967b79db696f909cff89ea9a", k.hex()
    assert vehicle.public_bytes.hex().startswith("04c7a1f47138486aa4729971494878d33b")
    assert client_pub_bytes(TEST_CLIENT_PRIVATE_KEY).hex().startswith("04b2b6bc68c2da06")
    print("[OK] 官方 ECDH / 公钥测试向量一致, K =", k.hex())


def test_official_session_info_hmac():
    vehicle = VehicleIdentity(TEST_VEHICLE_PRIVATE_KEY)
    client_priv = ec.derive_private_key(TEST_CLIENT_PRIVATE_KEY, ec.SECP256R1())
    k = derive_shared_key(client_priv.exchange(ec.ECDH(), vehicle.public_key))
    session_info_key = derive_session_info_key(k)
    assert session_info_key.hex() == (
        "fceb679ee7bca756fcd441bf238bf2f338629b41d9eb9c67be1b32c9672ce300"
    ), session_info_key.hex()
    session_info = signatures_pb2.SessionInfo()
    session_info.counter = 6
    session_info.publicKey = vehicle.public_bytes
    session_info.epoch = bytes.fromhex("4c463f9cc0d3d26906e982ed224adde6")
    session_info.clock_time = 2650
    raw = session_info.SerializeToString()
    assert raw.hex() == (
        "0806124104c7a1f47138486aa4729971494878d33b1a24e39571f748a6e16c5955b3d8"
        "77d3a6aaa0e955166474af5d32c410f439a2234137ad1bb085fd4e8813c958f11d971a"
        "104c463f9cc0d3d26906e982ed224adde6255a0a0000"
    ), raw.hex()
    uuid = bytes.fromhex("1588d5a30eabc6f8fc9a951b11f6fd11")
    meta = session_info_metadata(VIN, uuid)
    assert meta.hex() == (
        "000106021135594a333031323334353637383941424306101588d5a30eabc6f8fc9a951b"
        "11f6fd11ff"
    ), meta.hex()
    tag = hmac_sha256(session_info_key, meta + raw)
    assert tag.hex() == "996c1fe38331be138f8039c194b14db2198846ed7d8251e6749284d7b32ea002", tag.hex()
    print("[OK] 官方 session info TLV/HMAC 测试向量一致")


def test_end_to_end():
    server = TeslaCryptoServer(VIN, logger=lambda m: print("   ", m))
    client_priv = ec.derive_private_key(TEST_CLIENT_PRIVATE_KEY, ec.SECP256R1())
    client_pub = encode_public(client_priv.public_key())
    routing = secrets.token_bytes(16)
    uuid = secrets.token_bytes(16)

    # ── 握手请求 ──
    req = universal_message_pb2.RoutableMessage()
    req.to_destination.domain = DOMAIN_INFOTAINMENT
    req.from_destination.routing_address = routing
    req.session_info_request.public_key = client_pub
    req.uuid = uuid

    replies = server.feed(frame(req.SerializeToString()))
    assert len(replies) == 1
    resp = universal_message_pb2.RoutableMessage()
    resp.ParseFromString(unframe(replies[0]))
    assert resp.HasField("session_info")
    assert resp.request_uuid == uuid

    session_info = signatures_pb2.SessionInfo()
    session_info.ParseFromString(resp.session_info)
    k = derive_shared_key(client_priv.exchange(ec.ECDH(), VehicleIdentity(TEST_VEHICLE_PRIVATE_KEY).public_key))
    session_info_key = derive_session_info_key(k)
    meta = session_info_metadata(VIN, uuid)
    expected_tag = hmac_sha256(session_info_key, meta + resp.session_info)
    assert expected_tag == resp.signature_data.session_info_tag.tag, "session info HMAC 校验失败"
    print(f"[OK] 握手响应校验通过 counter={session_info.counter} epoch={session_info.epoch.hex()} clock={session_info.clock_time}")

    # ── GetVehicleData 加密命令 ──
    action = car_server_pb2.Action()
    action.vehicleAction.getVehicleData.getChargeState.SetInParent()
    action.vehicleAction.getVehicleData.getDriveState.SetInParent()
    action.vehicleAction.getVehicleData.getClimateState.SetInParent()
    plaintext = action.SerializeToString()

    counter = session_info.counter + 1
    expires_at = int(time.time()) + 30
    flags = FLAG_ENCRYPT_RESPONSE
    metadata = request_metadata(
        DOMAIN_INFOTAINMENT, VIN, session_info.epoch, counter, expires_at, flags
    )
    nonce = os.urandom(12)
    sealed = AESGCM(k).encrypt(nonce, plaintext, checksum(metadata))
    ciphertext, tag = sealed[:-16], sealed[-16:]

    cmd = universal_message_pb2.RoutableMessage()
    cmd.to_destination.domain = DOMAIN_INFOTAINMENT
    cmd.from_destination.routing_address = routing
    cmd.protobuf_message_as_bytes = ciphertext
    cmd.signature_data.signer_identity.public_key = client_pub
    cmd.signature_data.AES_GCM_Personalized_data.epoch = session_info.epoch
    cmd.signature_data.AES_GCM_Personalized_data.nonce = nonce
    cmd.signature_data.AES_GCM_Personalized_data.counter = counter
    cmd.signature_data.AES_GCM_Personalized_data.expires_at = expires_at
    cmd.signature_data.AES_GCM_Personalized_data.tag = tag
    cmd.flags = flags
    cmd.uuid = secrets.token_bytes(16)

    replies = server.feed(frame(cmd.SerializeToString()))
    assert len(replies) == 1
    rresp = universal_message_pb2.RoutableMessage()
    rresp.ParseFromString(unframe(replies[0]))
    assert rresp.signedMessageStatus.signed_message_fault == 0, rresp.signedMessageStatus
    assert rresp.HasField("signature_data") and rresp.signature_data.WhichOneof("sig_type") == "AES_GCM_Response_data"

    rdata = rresp.signature_data.AES_GCM_Response_data
    request_hash = build_request_hash(SIG_TYPE_AES_GCM_PERSONALIZED, tag, DOMAIN_INFOTAINMENT)
    rmeta = response_metadata(
        DOMAIN_INFOTAINMENT, VIN, rdata.counter, rresp.flags, request_hash, 0
    )
    rplain = AESGCM(k).decrypt(
        rdata.nonce, rresp.protobuf_message_as_bytes + rdata.tag, checksum(rmeta)
    )
    car_resp = car_server_pb2.Response()
    car_resp.ParseFromString(rplain)
    assert car_resp.actionStatus.result == car_server_pb2.OPERATIONSTATUS_OK
    print(
        f"[OK] 加密命令往返成功: SOC={car_resp.vehicleData.charge_state.battery_level}% "
        f"speed={car_resp.vehicleData.drive_state.speed_float}km/h "
        f"inside={car_resp.vehicleData.climate_state.inside_temp_celsius}C "
        f"response_counter={rdata.counter}"
    )


def _handshake(server, client_pub, domain=DOMAIN_VEHICLE_SECURITY):
    """发起一次握手，返回 (RoutableMessage, SessionInfo, routing_address, uuid)。"""
    uuid = secrets.token_bytes(16)
    routing = secrets.token_bytes(16)
    req = universal_message_pb2.RoutableMessage()
    req.to_destination.domain = domain
    req.from_destination.routing_address = routing
    req.session_info_request.public_key = client_pub
    req.uuid = uuid
    replies = server.feed(frame(req.SerializeToString()))
    assert len(replies) == 1, f"握手应只有 1 条回包，实际 {len(replies)}"
    resp = universal_message_pb2.RoutableMessage()
    resp.ParseFromString(unframe(replies[0]))
    info = signatures_pb2.SessionInfo()
    info.ParseFromString(resp.session_info)
    return resp, info, routing, uuid


def _send_vcsec_encrypted(server, client_priv, client_pub, session_info, routing, unsigned, flags=0):
    """用 AES-GCM 会话加密发送 VCSEC.UnsignedMessage，返回解析后的 FromVCSECMessage。"""
    k = derive_shared_key(
        client_priv.exchange(ec.ECDH(), VehicleIdentity(TEST_VEHICLE_PRIVATE_KEY).public_key)
    )
    counter = session_info.counter + 1
    expires_at = int(time.time()) + 30
    metadata = request_metadata(
        DOMAIN_VEHICLE_SECURITY, VIN, session_info.epoch, counter, expires_at, flags
    )
    nonce = os.urandom(12)
    sealed = AESGCM(k).encrypt(nonce, unsigned.SerializeToString(), checksum(metadata))
    ciphertext, tag = sealed[:-16], sealed[-16:]

    cmd = universal_message_pb2.RoutableMessage()
    cmd.to_destination.domain = DOMAIN_VEHICLE_SECURITY
    cmd.from_destination.routing_address = routing
    cmd.protobuf_message_as_bytes = ciphertext
    cmd.signature_data.signer_identity.public_key = client_pub
    cmd.signature_data.AES_GCM_Personalized_data.epoch = session_info.epoch
    cmd.signature_data.AES_GCM_Personalized_data.nonce = nonce
    cmd.signature_data.AES_GCM_Personalized_data.counter = counter
    cmd.signature_data.AES_GCM_Personalized_data.expires_at = expires_at
    cmd.signature_data.AES_GCM_Personalized_data.tag = tag
    cmd.flags = flags
    cmd.uuid = secrets.token_bytes(16)

    replies = server.feed(frame(cmd.SerializeToString()))
    assert len(replies) == 1, f"命令应只有 1 条回包，实际 {len(replies)}"
    resp = universal_message_pb2.RoutableMessage()
    resp.ParseFromString(unframe(replies[0]))
    assert resp.signedMessageStatus.signed_message_fault == 0, resp.signedMessageStatus
    assert not (flags & FLAG_ENCRYPT_RESPONSE), "本测试助手只处理明文响应"
    out = vcsec_pb2.FromVCSECMessage()
    out.ParseFromString(resp.protobuf_message_as_bytes)
    return out


def _add_key_unsigned(public_key, role):
    unsigned = vcsec_pb2.UnsignedMessage()
    change = unsigned.WhitelistOperation.addKeyToWhitelistAndAddPermissions
    change.key.PublicKeyRaw = public_key
    change.keyRole = role
    unsigned.WhitelistOperation.metadataForKey.keyFormFactor = (
        vcsec_pb2.KEY_FORM_FACTOR_ANDROID_DEVICE
    )
    return unsigned


def test_pairing_add_key_request():
    """未配对 -> add-key-request 明文信封 -> WAIT -> 刷卡 -> 终态 OK -> 握手成功。"""
    vehicle = VehicleIdentity(TEST_VEHICLE_PRIVATE_KEY)
    owner_priv = ec.derive_private_key(TEST_CLIENT_PRIVATE_KEY, ec.SECP256R1())
    owner_pub = encode_public(owner_priv.public_key())
    new_priv = ec.generate_private_key(ec.SECP256R1())
    new_pub = encode_public(new_priv.public_key())

    server = TeslaCryptoServer(
        VIN, logger=lambda m: print("   ", m), allowed_client_public=owner_pub
    )
    assert server.enforce_whitelist

    # 1) 未配对公钥握手 -> SessionInfo status=KEY_NOT_ON_WHITELIST（不是协议 fault）
    resp, info, _, uuid = _handshake(server, new_pub)
    assert info.status == signatures_pb2.SESSION_INFO_STATUS_KEY_NOT_ON_WHITELIST, info
    assert not resp.HasField("signature_data"), "未配对响应不应带 HMAC tag"
    assert resp.request_uuid == uuid
    print("[OK] 未配对握手回 SessionInfo status=KEY_NOT_ON_WHITELIST")

    # 2) 明文 add-key-request -> OPERATIONSTATUS_WAIT（车机提示「中控台轻点钥匙卡」）
    envelope = vcsec_pb2.ToVCSECMessage()
    envelope.signedMessage.protobufMessageAsBytes = _add_key_unsigned(
        new_pub, keys_pb2.ROLE_OWNER
    ).SerializeToString()
    envelope.signedMessage.signatureType = vcsec_pb2.SIGNATURE_TYPE_PRESENT_KEY
    raw_envelope = envelope.SerializeToString()
    assert raw_envelope[:1] == b"\x0a", "ToVCSECMessage 首字节应为 signedMessage tag"

    replies = server.feed(frame(raw_envelope))
    assert len(replies) == 1
    vr = universal_message_pb2.RoutableMessage()
    vr.ParseFromString(unframe(replies[0]))
    assert int(vr.from_destination.domain) == DOMAIN_VEHICLE_SECURITY
    vresp = vcsec_pb2.FromVCSECMessage()
    vresp.ParseFromString(vr.protobuf_message_as_bytes)
    assert vresp.commandStatus.operationStatus == OPERATIONSTATUS_WAIT, vresp.commandStatus
    assert server.pairing_pending() is not None
    print("[OK] add-key-request 返回 WAIT，等待 NFC 刷卡")

    # 3) 模拟中控台刷卡 -> 终态 WhitelistOperation_status
    assert server.pairing_approve(), "应存在待批准配钥请求"
    events = server.poll_events()
    assert len(events) == 1, f"刷卡后应产生 1 条回包，实际 {len(events)}"
    fr = universal_message_pb2.RoutableMessage()
    fr.ParseFromString(unframe(events[0]))
    fresp = vcsec_pb2.FromVCSECMessage()
    fresp.ParseFromString(fr.protobuf_message_as_bytes)
    status = fresp.commandStatus
    assert status.operationStatus == OPERATIONSTATUS_OK, status
    wl = status.whitelistOperationStatus
    assert wl.whitelistOperationInformation == INFO_NONE, wl
    assert wl.signerOfOperation.publicKeySHA1 == hashlib.sha1(new_pub).digest()
    assert server.whitelist.contains(new_pub)
    entry = server.whitelist.find(new_pub)
    print(f"[OK] 刷卡授权成功 slot={entry.slot} role={entry.role}")

    # 4) 新公钥重新握手 -> status OK 且 HMAC tag 可验证
    resp, info, _, uuid = _handshake(server, new_pub)
    assert info.status == signatures_pb2.SESSION_INFO_STATUS_OK, info
    k = derive_shared_key(new_priv.exchange(ec.ECDH(), vehicle.public_key))
    meta = session_info_metadata(VIN, uuid)
    expected = hmac_sha256(derive_session_info_key(k), meta + resp.session_info)
    assert expected == resp.signature_data.session_info_tag.tag, "配钥后 SessionInfo HMAC 校验失败"
    print("[OK] 配钥后新公钥握手成功，SessionInfo HMAC 校验通过")


def test_encrypted_add_key():
    """已配对 OWNER 走加密 add-key；DRIVER 无权加钥。"""
    owner_priv = ec.derive_private_key(TEST_CLIENT_PRIVATE_KEY, ec.SECP256R1())
    owner_pub = encode_public(owner_priv.public_key())
    target_priv = ec.generate_private_key(ec.SECP256R1())
    target_pub = encode_public(target_priv.public_key())
    other_pub = encode_public(ec.generate_private_key(ec.SECP256R1()).public_key())

    server = TeslaCryptoServer(
        VIN, logger=lambda m: print("   ", m), allowed_client_public=owner_pub
    )
    _, owner_info, owner_routing, _ = _handshake(server, owner_pub)
    assert owner_info.status == signatures_pb2.SESSION_INFO_STATUS_OK

    out = _send_vcsec_encrypted(
        server, owner_priv, owner_pub, owner_info, owner_routing,
        _add_key_unsigned(target_pub, keys_pb2.ROLE_DRIVER),
    )
    assert out.commandStatus.whitelistOperationStatus.whitelistOperationInformation == INFO_NONE
    assert server.whitelist.find(target_pub).role == keys_pb2.ROLE_DRIVER
    print("[OK] OWNER 加密 add-key 成功（DRIVER 角色已写入白名单）")

    # DRIVER 尝试继续加钥 -> 无权限
    _, driver_info, driver_routing, _ = _handshake(server, target_pub)
    assert driver_info.status == signatures_pb2.SESSION_INFO_STATUS_OK
    out = _send_vcsec_encrypted(
        server, target_priv, target_pub, driver_info, driver_routing,
        _add_key_unsigned(other_pub, keys_pb2.ROLE_OWNER),
    )
    got = out.commandStatus.whitelistOperationStatus.whitelistOperationInformation
    assert got == INFO_NO_PERMISSION_TO_ADD, got
    assert not server.whitelist.contains(other_pub)
    print("[OK] DRIVER 加钥被拒绝 info=NO_PERMISSION_TO_ADD")


if __name__ == "__main__":
    test_official_ecdh()
    test_official_session_info_hmac()
    test_end_to_end()
    test_pairing_add_key_request()
    test_encrypted_add_key()
    print("ALL CRYPTO TESTS PASSED")
