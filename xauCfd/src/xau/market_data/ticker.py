"""Ticker snapshot from the Gate.io TradFi REST API.

The ticker endpoint returns the latest last/bid/ask plus today's session
high/low/open. For the volume-profile reversion strategy, `last_price` is
the input to bar aggregation, and `today_high` / `today_low` give us an
independent sanity check on the synthetic profile's range.

Field coercion:
  * All numeric fields are returned as decimal STRINGS by the server (e.g.
    "4368.29"). We preserve them as strings to avoid float drift across
    the bar aggregator. Conversion to Decimal/Int happens at the analysis
    edge, not in this layer.
  * `close_time` is a UNIX timestamp in **seconds** (observed 1754000000
    range). The XAU market is 23/5 so it should never be in the past by
    more than a weekend.
"""
from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from xau.gate.client import GateClient


@dataclass(frozen=True, slots=True)
class Ticker:
    """Snapshot of the XAUUSD TradFi CFD ticker."""
    symbol: str
    last_price: str
    bid_price: str
    ask_price: str
    today_open: str
    today_high: str
    today_low: str
    price_change_pct: str
    close_time_s: int  # server-reported next session close (UNIX seconds)

    @classmethod
    def from_payload(cls, symbol: str, data: dict[str, Any]) -> Ticker:
        return cls(
            symbol=symbol,
            last_price=str(data.get("last_price", "0")),
            bid_price=str(data.get("bid_price", "0")),
            ask_price=str(data.get("ask_price", "0")),
            today_open=str(data.get("today_open_price", "0")),
            today_high=str(data.get("highest_price", "0")),
            today_low=str(data.get("lowest_price", "0")),
            price_change_pct=str(data.get("price_change", "0")),
            close_time_s=int(data.get("close_time", 0) or 0),
        )

    def mid(self) -> str:
        """Average of bid and ask. Returned as string to preserve precision."""
        from decimal import Decimal
        return str((Decimal(self.bid_price) + Decimal(self.ask_price)) / 2)


def fetch_ticker(client: GateClient, symbol: str = "XAUUSD") -> Ticker:
    """GET /tradfi/symbols/{symbol}/tickers — returns the latest snapshot."""
    payload = client.request("GET", f"/tradfi/symbols/{symbol}/tickers")
    if not isinstance(payload, dict):
        raise ValueError(f"unexpected ticker payload type: {type(payload).__name__}")
    return Ticker.from_payload(symbol, payload)