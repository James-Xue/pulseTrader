"""Gate.io TradFi product spec queries (public endpoints).

The `/tradfi/symbols` endpoint is unauthenticated — no signing required.
Returns a `data.list` of all TradFi products. We filter to the symbol
of interest (XAUUSD) and surface only the fields the strategy uses.
"""
from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from xau.gate.client import GateClient


@dataclass(frozen=True, slots=True)
class TradFiProduct:
    """Subset of the TradFi symbol spec the strategy cares about."""
    symbol: str
    description: str
    category_id: int
    trade_mode: int
    status: str
    price_precision: int
    settlement_currency: str
    leverages: tuple[int, ...]

    @classmethod
    def from_payload(cls, p: dict[str, Any]) -> TradFiProduct:
        levs_raw = p.get("leverages") or []
        levs: tuple[int, ...] = ()
        if isinstance(levs_raw, list):
            parsed: list[int] = []
            for x in levs_raw:
                try:
                    # Handle "100", "3.33", 100, 100.0 — server is inconsistent.
                    parsed.append(int(float(x)))
                except (TypeError, ValueError):
                    continue
            levs = tuple(parsed)
        return cls(
            symbol=p.get("symbol", ""),
            description=p.get("symbol_desc", ""),
            category_id=int(p.get("category_id", 0)),
            trade_mode=int(p.get("trade_mode", 0)),
            status=str(p.get("status", "")),
            price_precision=int(p.get("price_precision", 0)),
            settlement_currency=p.get("settlement_currency", ""),
            leverages=levs,
        )


def list_symbols(client: GateClient) -> list[TradFiProduct]:
    """GET /tradfi/symbols — all TradFi CFD products (no auth)."""
    raw = client.request("GET", "/tradfi/symbols", signed=False)
    items = (raw or {}).get("list") if isinstance(raw, dict) else raw
    if not isinstance(items, list):
        return []
    return [TradFiProduct.from_payload(p) for p in items]


def get_symbol(client: GateClient, symbol: str) -> TradFiProduct | None:
    """Look up a single product by symbol. Returns None if not found."""
    for p in list_symbols(client):
        if p.symbol == symbol:
            return p
    return None