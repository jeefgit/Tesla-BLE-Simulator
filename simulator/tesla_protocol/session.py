"""每域会话状态与密钥派生。"""

from __future__ import annotations

import hashlib
import hmac
import secrets
import time
from dataclasses import dataclass, field

SESSION_INFO_LABEL = b"session info"
AUTHENTICATED_COMMAND_LABEL = b"authenticated command"

DOMAIN_VEHICLE_SECURITY = 2
DOMAIN_INFOTAINMENT = 3

# Infotainment 允许乱序的滑动窗口大小（VCSEC 严格要求顺序）
INFOTAINMENT_WINDOW = 64


def derive_shared_key(shared_x: bytes) -> bytes:
    """K = SHA1(BIG_ENDIAN(Sx, 32))[:16]。"""
    if len(shared_x) != 32:
        raise ValueError(f"共享点 x 坐标必须为 32 字节，实际 {len(shared_x)}")
    return hashlib.sha1(shared_x).digest()[:16]


def derive_session_info_key(shared_key: bytes) -> bytes:
    """SESSION_INFO_KEY = HMAC-SHA256(K, "session info")。"""
    return hmac.new(shared_key, SESSION_INFO_LABEL, hashlib.sha256).digest()


def derive_command_key(shared_key: bytes) -> bytes:
    """K' = HMAC-SHA256(K, "authenticated command")（HMAC 命令模式用）。"""
    return hmac.new(shared_key, AUTHENTICATED_COMMAND_LABEL, hashlib.sha256).digest()


def new_epoch() -> bytes:
    return secrets.token_bytes(16)


@dataclass
class DomainSession:
    """单个域（VCSEC / Infotainment）的会话状态。"""

    domain: int
    client_public: bytes
    shared_key: bytes
    session_info_key: bytes
    command_key: bytes
    epoch: bytes
    clock_offset: int          # 车端时钟 - 本机墙钟（秒）
    initial_counter: int       # SessionInfo.counter，同时是响应计数器起点
    routing_address: bytes
    created_at: float = field(default_factory=time.time)

    last_client_counter: int = 0
    response_counter: int = 0
    seen_counters: set[int] = field(default_factory=set)
    request_count: int = 0

    def __post_init__(self) -> None:
        self.last_client_counter = self.initial_counter
        self.response_counter = self.initial_counter
        self.seen_counters = set()

    # ── 计数器 ──────────────────────────────────────────────
    def accepts_counter(self, counter: int) -> bool:
        """防重放校验：VCSEC 严格递增，Infotainment 使用滑动窗口。"""
        if counter < 0:
            return False
        if self.domain == DOMAIN_VEHICLE_SECURITY:
            return counter > self.last_client_counter
        if counter <= self.last_client_counter - INFOTAINMENT_WINDOW:
            return False
        if counter in self.seen_counters:
            return False
        return counter > self.initial_counter

    def accept_counter(self, counter: int) -> None:
        self.last_client_counter = max(self.last_client_counter, counter)
        self.seen_counters.add(counter)
        if len(self.seen_counters) > INFOTAINMENT_WINDOW * 2:
            floor = self.last_client_counter - INFOTAINMENT_WINDOW
            self.seen_counters = {c for c in self.seen_counters if c > floor}
        self.request_count += 1

    def next_response_counter(self) -> int:
        self.response_counter += 1
        return self.response_counter

    # ── 时钟 ──────────────────────────────────────────────
    def domain_now(self, now_wall: float | None = None) -> int:
        now_wall = time.time() if now_wall is None else now_wall
        return int(now_wall) + self.clock_offset

    def summary(self) -> dict:
        return {
            "domain": self.domain,
            "client_public": self.client_public.hex(),
            "key_fingerprint": self.shared_key.hex()[:8],
            "epoch": self.epoch.hex(),
            "counter": self.last_client_counter,
            "response_counter": self.response_counter,
            "requests": self.request_count,
            "age_s": round(time.time() - self.created_at, 1),
        }
