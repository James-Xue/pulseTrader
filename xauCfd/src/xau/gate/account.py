"""Gate.io TradFi account / mt5-account / assets queries.

Verified payload shapes (from probe 2026-08-11):

    GET /tradfi/users/mt5-account →
        { is_register: bool, mt5_uid: int, leverage: int,
          server: str, stop_out_level: str, status: int }

    GET /tradfi/users/assets →
        { equity: str, margin_level: str, balance: str,
          margin: str, margin_free: str, outable: str,
          unrealized_pnl: str, storage: str, mt5_uid: str,
          messages: Any, trial_used_amount: str }

Note: balance/equity etc. arrive as **decimal strings** (not floats) — the
server carries more precision than JSON numbers can preserve. Convert at
the boundary, not in the wire format.
"""
from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from xau.gate.client import GateClient


@dataclass(frozen=True, slots=True)
class Mt5Account:
    is_register: bool
    mt5_uid: int
    leverage: int          # account-level display default; per-order leverage overrides
    server: str
    stop_out_level: str
    status: int

    @classmethod
    def from_payload(cls, p: dict[str, Any]) -> Mt5Account:
        return cls(
            is_register=bool(p.get("is_register", False)),
            mt5_uid=int(p.get("mt5_uid", 0)),
            leverage=int(p.get("leverage", 0)),
            server=str(p.get("server", "")),
            stop_out_level=str(p.get("stop_out_level", "")),
            status=int(p.get("status", 0)),
        )


@dataclass(frozen=True, slots=True)
class AccountAssets:
    """All values are decimal strings — convert at the edge."""
    equity: str
    balance: str
    margin: str
    margin_free: str
    outable: str            # withdrawable amount
    unrealized_pnl: str
    storage: str            # overnight carry (typically 0 for gold CFD)
    margin_level: str       # % ; "0.00" when no open positions
    trial_used_amount: str
    mt5_uid: str

    @classmethod
    def from_payload(cls, p: dict[str, Any]) -> AccountAssets:
        def s(k: str) -> str:
            return str(p.get(k, "0"))
        return cls(
            equity=s("equity"),
            balance=s("balance"),
            margin=s("margin"),
            margin_free=s("margin_free"),
            outable=s("outable"),
            unrealized_pnl=s("unrealized_pnl"),
            storage=s("storage"),
            margin_level=s("margin_level"),
            trial_used_amount=s("trial_used_amount"),
            mt5_uid=s("mt5_uid"),
        )


def get_mt5_account(client: GateClient) -> Mt5Account:
    """GET /tradfi/users/mt5-account — broker account registration info."""
    raw = client.request("GET", "/tradfi/users/mt5-account")
    return Mt5Account.from_payload(raw)


def get_assets(client: GateClient) -> AccountAssets:
    """GET /tradfi/users/assets — equity, balance, margin snapshot."""
    raw = client.request("GET", "/tradfi/users/assets")
    return AccountAssets.from_payload(raw)