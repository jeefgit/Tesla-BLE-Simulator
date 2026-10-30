"""车端白名单（VCSEC key store）的最小模型。

真车把每个已登记钥匙的公钥、角色、形态与槽位存在 VCSEC 里：
- 握手时用它判断客户端公钥是否已配对（不在白名单 -> SessionInfo
  status = SESSION_INFO_STATUS_KEY_NOT_ON_WHITELIST）；
- WhitelistOperation 用它增删钥匙 / 改权限；
- InformationRequest 用它回 GET_WHITELIST_INFO / GET_WHITELIST_ENTRY_INFO。

字段号与枚举见 tesla_protocol/protobuf/vcsec.proto 与 keys.proto。
"""

from __future__ import annotations

import hashlib
from dataclasses import dataclass

from .pb import keys_pb2, vcsec_pb2

# VCSEC 的钥匙槽位数量有限（真车约 8~10 个），模拟器取 8 便于测试「白名单已满」
MAX_ENTRIES = 8


@dataclass
class WhitelistEntry:
    public_key: bytes
    role: int = keys_pb2.ROLE_OWNER
    form_factor: int = vcsec_pb2.KEY_FORM_FACTOR_ANDROID_DEVICE
    slot: int = 1

    @property
    def sha1(self) -> bytes:
        return hashlib.sha1(self.public_key).digest()


class Whitelist:
    def __init__(self, logger=None):
        self._entries: list[WhitelistEntry] = []
        self.log = logger or (lambda msg: None)

    # ── 查询 ───────────────────────────────────────────────
    def __len__(self) -> int:
        return len(self._entries)

    def entries(self) -> list[WhitelistEntry]:
        return list(self._entries)

    def find(self, public_key: bytes) -> WhitelistEntry | None:
        for entry in self._entries:
            if entry.public_key == public_key:
                return entry
        return None

    def find_by_sha1(self, digest: bytes) -> WhitelistEntry | None:
        for entry in self._entries:
            if entry.sha1 == digest:
                return entry
        return None

    def find_by_slot(self, slot: int) -> WhitelistEntry | None:
        for entry in self._entries:
            if entry.slot == slot:
                return entry
        return None

    def contains(self, public_key: bytes) -> bool:
        return self.find(public_key) is not None

    def is_full(self) -> bool:
        return len(self._entries) >= MAX_ENTRIES

    def slot_mask(self) -> int:
        mask = 0
        for entry in self._entries:
            if 1 <= entry.slot <= 32:
                mask |= 1 << (entry.slot - 1)
        return mask

    # ── 修改 ───────────────────────────────────────────────
    def _free_slot(self) -> int:
        used = {entry.slot for entry in self._entries}
        for slot in range(1, MAX_ENTRIES + 1):
            if slot not in used:
                return slot
        return 0

    def add(
        self,
        public_key: bytes,
        role: int = keys_pb2.ROLE_OWNER,
        form_factor: int = vcsec_pb2.KEY_FORM_FACTOR_ANDROID_DEVICE,
    ) -> WhitelistEntry | None:
        """登记钥匙；已存在时返回 None（对应 info=13 已在本白名单）。"""
        if self.find(public_key) is not None:
            return None
        slot = self._free_slot()
        if slot == 0:
            return None
        entry = WhitelistEntry(
            public_key=bytes(public_key), role=int(role), form_factor=int(form_factor), slot=slot
        )
        self._entries.append(entry)
        self._entries.sort(key=lambda e: e.slot)
        self.log(
            f"[VCSEC] 白名单新增 key={entry.sha1.hex()[:16]}… slot={entry.slot} "
            f"role={entry.role} formFactor={entry.form_factor}"
        )
        return entry

    def remove(self, public_key: bytes) -> bool:
        entry = self.find(public_key)
        if entry is None:
            return False
        self._entries.remove(entry)
        self.log(f"[VCSEC] 白名单移除 key={entry.sha1.hex()[:16]}… slot={entry.slot}")
        return True

    def set_role(self, public_key: bytes, role: int) -> bool:
        entry = self.find(public_key)
        if entry is None:
            return False
        entry.role = int(role)
        return True

    def clear(self) -> None:
        self._entries.clear()
