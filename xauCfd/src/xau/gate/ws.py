"""Gate.io TradFi WebSocket subscription.

The v4 WebSocket entry point is `wss://api.gateio.ws/ws/v4/`. Authenticated
subscriptions require the same `KEY`/`SIGN`/`Timestamp` headers (sent in
the OPEN handshake payload, NOT as HTTP headers).

This module is a **skeleton** — Phase 1 doesn't need WebSockets (we poll
REST). The shape is here so Phase 2 / live data work doesn't have to redo
the discovery.

Reference subscription channels for TradFi:
    - `tradfi.order_book` — order book deltas for a symbol
    - `tradfi.trades`     — public tape
    - `tradfi.candles`    — kline updates
    - `tradfi.positions`  — position updates (auth required)
    - `tradfi.orders`     — order updates (auth required)
    - `tradfi.balance`    — balance updates (auth required)

Verified via the WebFetch of gate.io's public API reference; we do NOT yet
have a working subscription round-trip — Phase 2 will harden this.
"""
from __future__ import annotations

import logging
from dataclasses import dataclass
from typing import Any

log = logging.getLogger("gate.ws")

DEFAULT_WS_URL = "wss://api.gateio.ws/ws/v4/"


@dataclass(frozen=True, slots=True)
class WsAuth:
    """Auth payload for WebSocket subscription. Computed the same way as REST."""
    api_key: str
    signature: str
    timestamp: str


def build_ws_auth(*, api_key: str, api_secret: str) -> WsAuth:
    """Build the WebSocket auth payload.

    The WS protocol uses the SAME HMAC canonical as REST, with the empty
    payload and the WS URL path as the `path`. Per the official SDK's
    WebSocket client (gate-api WSClient.subscribe), the `path` to sign
    is the subscription endpoint, e.g. `/ws/v4/`. We mirror that here.
    """
    from xau.gate.auth import sign
    import time as _time
    ts = repr(_time.time())
    sig = sign(api_secret, "GET", "/api/v4/ws/v4/", "", "", timestamp=ts)
    return WsAuth(api_key=api_key, signature=sig, timestamp=ts)


def auth_payload(auth: WsAuth) -> dict[str, Any]:
    """Auth dict shape sent in subscription payloads (per official WS SDK)."""
    return {
        "api_key": auth.api_key,
        "signature": auth.signature,
        "timestamp": auth.timestamp,
    }