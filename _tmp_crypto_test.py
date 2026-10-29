import os
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
from tesla_protocol.pb import car_server_pb2, signatures_pb2, universal_message_pb2
from tesla_protocol.secure_link import build_request_hash, frame, hmac_sha256, unframe
from tesla_protocol.server import FLAG_ENCRYPT_RESPONSE, TeslaCryptoServer
from tesla_protocol.session import (
    DOMAIN_INFOTAINMENT,
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


if __name__ == "__main__":
    test_official_ecdh()
    test_official_session_info_hmac()
    test_end_to_end()
    print("ALL CRYPTO TESTS PASSED")
