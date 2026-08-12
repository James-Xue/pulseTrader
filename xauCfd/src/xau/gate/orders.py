"""Gate.io TradFi order placement, listing, and cancellation.

Verified payload shapes (from probe 2026-08-11, 3 open orders):

    GET /tradfi/orders →
        { list: [Order, ...] }                       (no `total` field)

Each Order:
    order_id, symbol, base_symbol, leverage, symbol_desc, settlement_currency,
    exchange_rate, price_type ("trigger" = pending), state, state_desc,
    finished, side (1=sell, 2=buy per official CFD docs), volume, price,
    price_tp, price_sl, time_setup (unix seconds)

`state=1` + `finished=0` is "open pending". `finished=1` is terminal.
`price_type="trigger"` is what the 3 existing limit orders came back as.

POST /tradfi/orders request body (per official CFD schema, 2026-08-11):
    Required: symbol, side, volume, price, price_type ("trigger"|"market")
    Optional: price_tp, price_sl

This module is **read-only** — it lists and describes. Order placement
shapes are documented in the docstring but the implementation will land
in Phase 2 once we have a written integration test plan.
"""
from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Literal

from xau.gate.client import GateClient

PRICE_TYPE_MARKET = "market"
PRICE_TYPE_TRIGGER = "trigger"   # limit / stop / pending entry — what we use

# Per official CFD schema (gate.com/docs/developers/apiv4/en/cfd):
#   side=1 = SELL
#   side=2 = BUY
SIDE_SELL: Literal[1] = 1
SIDE_BUY: Literal[2] = 2

import logging
_log = logging.getLogger("gate.orders")


@dataclass(frozen=True, slots=True)
class Order:
    order_id: int
    symbol: str
    leverage: str
    price_type: str
    state: int          # 1 = open pending (observed)
    finished: int       # 0 = open, 1 = closed
    side: int           # 1 = sell, 2 = buy (per official CFD schema)
    volume: str
    price: str          # entry price for limit orders (decimal string)
    price_tp: str
    price_sl: str
    time_setup: int

    @classmethod
    def from_payload(cls, p: dict[str, Any]) -> Order:
        return cls(
            order_id=int(p.get("order_id", 0)),
            symbol=str(p.get("symbol", "")),
            leverage=str(p.get("leverage", "0")),
            price_type=str(p.get("price_type", "")),
            state=int(p.get("state", 0)),
            finished=int(p.get("finished", 0)),
            side=int(p.get("side", 0)),
            volume=str(p.get("volume", "0")),
            price=str(p.get("price", "0")),
            price_tp=str(p.get("price_tp", "0")),
            price_sl=str(p.get("price_sl", "0")),
            time_setup=int(p.get("time_setup", 0)),
        )


def list_orders(client: GateClient) -> list[Order]:
    """GET /tradfi/orders — all live orders (no filter param observed to work).

    The server returned 200 for both `/tradfi/orders` and `/tradfi/orders?status=open`
    in the probe; the latter may be intended for cancellation-filtering, but
    we default to the unfiltered list and let the caller filter client-side.
    """
    raw = client.request("GET", "/tradfi/orders")
    items = (raw or {}).get("list") if isinstance(raw, dict) else None
    if not isinstance(items, list):
        return []
    return [Order.from_payload(p) for p in items]


def list_open_pendings(client: GateClient, symbol: str | None = None) -> list[Order]:
    """Filter to orders that are still pending (state=1, finished=0) for symbol."""
    out = [o for o in list_orders(client) if o.state == 1 and o.finished == 0]
    if symbol:
        out = [o for o in out if o.symbol == symbol]
    return out


def place_pending(
    client: GateClient,
    *,
    symbol: str,
    side: int,
    volume: float,
    price: float,
    price_type: str = PRICE_TYPE_TRIGGER,
    price_tp: float | None = None,
    price_sl: float | None = None,
) -> dict[str, Any]:
    """POST /tradfi/orders — place a pending limit order.

    Per official CFD schema (gate.com/docs/developers/apiv4/en/cfd):
      Required: symbol, side, volume, price, price_type ("trigger"|"market")
      Optional: price_tp, price_sl

    Args:
      symbol: product symbol (e.g. "XAUUSD")
      side: 1=sell, 2=buy (per official CFD schema — REVERSED from earlier)
      volume: lots (XAU standard lot = 100 oz; 0.01 = 1 oz)
      price: limit price (decimal)
      price_type: 'trigger' (limit/stop pending) or 'market'
      price_tp: take-profit (optional)
      price_sl: stop-loss (optional)

    Returns the raw response envelope (`data` field). Phase 2 integration
    test will assert the return shape; for now we trust the server.

    NOTE: leverage is NOT a request field — broker uses account's per-order
    leverage from account settings. Sending leverage here caused
    INVALID_ARGUMENT (probed 2026-08-11).
    """
    body: dict[str, Any] = {
        "symbol": symbol,
        "side": side,
        "volume": str(volume),
        "price": str(price),
        "price_type": price_type,
    }
    if price_tp is not None:
        body["price_tp"] = f"{price_tp:.2f}"
    if price_sl is not None:
        body["price_sl"] = f"{price_sl:.2f}"
    _log.info("place_pending body=%s", body)
    return client.request("POST", "/tradfi/orders", body=body)


def cancel_order(client: GateClient, order_id: int) -> dict[str, Any]:
    """DELETE /tradfi/orders/{order_id} — cancel one pending order.

    Server behavior not yet observed live. Expect 200 with empty data on
    success; 4xx if order is already filled or unknown.
    """
    return client.request("DELETE", f"/tradfi/orders/{order_id}")


def cancel_all_pendings(client: GateClient, symbol: str | None = None) -> list[Order]:
    """Cancel every open pending. Returns the list that was cancelled.

    Useful for emergency-flatten or pre-test cleanup. Note: this is racy
    if fills arrive between `list` and `cancel` — caller should treat
    the returned list as "best-effort cancelled".
    """
    pending = list_open_pendings(client, symbol)
    cancelled: list[Order] = []
    for o in pending:
        try:
            cancel_order(client, o.order_id)
            cancelled.append(o)
        except Exception:
            # Already filled, already cancelled, or transient error — skip.
            continue
    return cancelled