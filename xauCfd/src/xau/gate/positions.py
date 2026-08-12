"""Gate.io TradFi position queries + close.

Verified payload (from probe 2026-08-11):
    GET /tradfi/positions →
        { total: int, total_page: int, list: [Position, ...] | null }

Each Position (subset of fields we use):
    position_id, symbol, position_dir ("Long" | "Short"), volume,
    price_open, leverage, margin, unrealized_pnl, time_create

NOTE: position payload does NOT include numeric `side` — direction is
encoded as `position_dir` string. `from_payload` maps "Long"→2 (BUY),
"Short"→1 (SELL) to match order-side semantics
(gate.com/docs/developers/apiv4/en/cfd).

NOTE: a closed/flat account returns `list: null` (not `[]`).

Close endpoint (gate.com/docs/developers/apiv4/en/cfd):
    POST /tradfi/positions/{position_id}/close
    body: {"close_type": 1|2, "close_volume": "X"}
    close_type=1 partial (requires close_volume), close_type=2 full

WARNING: market orders do NOT net/close — POST /tradfi/orders with
price_type='market' opens a NEW position at the opposite direction.
Closing always goes through the close endpoint below.
"""
from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Literal

from xau.gate.client import GateClient


SIDE_SELL: Literal[1] = 1
SIDE_BUY: Literal[2] = 2

# Map position_dir → side int (matches CFD order-side convention).
_POSITION_DIR_TO_SIDE: dict[str, int] = {
    "Long": SIDE_BUY,
    "Short": SIDE_SELL,
}

CLOSE_TYPE_PARTIAL = 1   # requires close_volume
CLOSE_TYPE_FULL = 2      # ignores close_volume


@dataclass(frozen=True, slots=True)
class Position:
    position_id: int
    symbol: str
    side: int          # 2=buy (Long), 1=sell (Short) — derived from position_dir
    volume: str        # decimal string — lots
    price: str         # decimal string — average fill price (from price_open)
    leverage: str      # decimal string — per-order leverage
    profit: str        # decimal string — unrealized PnL (from unrealized_pnl)
    margin: str        # decimal string — used margin
    time_setup: int    # unix seconds (from time_create)

    @classmethod
    def from_payload(cls, p: dict[str, Any]) -> Position:
        # The broker encodes direction as position_dir; older payloads may
        # carry a numeric `side` (kept here for forward-compat). Map to int.
        direction = p.get("position_dir") or p.get("side")
        if isinstance(direction, str):
            side = _POSITION_DIR_TO_SIDE.get(direction, 0)
        elif isinstance(direction, (int, float)):
            side = int(direction)
        else:
            side = 0
        return cls(
            position_id=int(p.get("position_id", 0)),
            symbol=str(p.get("symbol", "")),
            side=side,
            volume=str(p.get("volume", "0")),
            price=str(p.get("price_open", p.get("price", "0"))),
            leverage=str(p.get("leverage", "0")),
            profit=str(p.get("unrealized_pnl", p.get("profit", "0"))),
            margin=str(p.get("margin", "0")),
            time_setup=int(p.get("time_create", p.get("time_setup", 0))),
        )


def list_positions(
    client: GateClient,
    symbol: str | None = None,
) -> list[Position]:
    """GET /tradfi/positions — current open positions.

    `symbol=None` returns all; passing `symbol="XAUUSD"` filters server-side.
    Returns [] when account is flat (the server returns `list: null`).
    """
    params: dict[str, Any] | None = {"symbol": symbol} if symbol else None
    raw = client.request("GET", "/tradfi/positions", params=params)
    items = (raw or {}).get("list") if isinstance(raw, dict) else None
    if not isinstance(items, list):
        return []
    return [Position.from_payload(p) for p in items]


def close_position(
    client: GateClient,
    position_id: int,
    *,
    close_type: int = CLOSE_TYPE_FULL,
    close_volume: str | None = None,
) -> dict[str, Any]:
    """POST /tradfi/positions/{position_id}/close — close an open position.

    Args:
      close_type: 1 = partial (requires close_volume), 2 = full (default).
      close_volume: lots to close when close_type=1. Decimal string.

    Returns the raw response envelope (typically {"log_id": "..."}).

    WARNING: do NOT try to close by placing a market order in the opposite
    direction — broker treats that as a fresh position open, leaving the
    original exposed and adding a new one.
    """
    body: dict[str, Any] = {"close_type": close_type}
    if close_type == CLOSE_TYPE_PARTIAL:
        if close_volume is None:
            raise ValueError("close_volume required for partial close")
        body["close_volume"] = close_volume
    return client.request("POST", f"/tradfi/positions/{position_id}/close", body=body)


def update_position_sl_tp(
    client: GateClient,
    position_id: int,
    *,
    price_sl: str | None,
    price_tp: str | None,
) -> dict[str, Any]:
    """PUT /tradfi/positions/{position_id} — set or clear TP/SL on a position.

    Per official CFD schema (gate.com/docs/developers/apiv4/en/cfd):
        PUT /tradfi/positions/{position_id}
        body: {"price_tp": "...", "price_sl": "..."}

    Both fields are optional and may be None. The broker interprets omitted
    or "0" values as "clear that side"; pass the existing value to keep it.

    Args:
      price_sl: stop-loss price as a decimal string (e.g. "4389.87"),
                or None to leave the existing SL untouched.
      price_tp: take-profit price as a decimal string, or None.

    Returns the raw response envelope (typically empty `data` on success).
    """
    body: dict[str, Any] = {}
    if price_sl is not None:
        body["price_sl"] = price_sl
    if price_tp is not None:
        body["price_tp"] = price_tp
    return client.request("PUT", f"/tradfi/positions/{position_id}", body=body)