#!/usr/bin/env python3
"""Demo control-plane engine.

Serves the same wire contract as the real engine — newline-delimited
JSON-RPC 2.0 on TCP — but with fixed sample payloads. It exists so that a
newcomer can connect a client and see the whole surface working in under a
minute, without exchange credentials, without a funded account and without
touching the C++ build.

It is a fixture, not a simulator: nothing here is computed from market data.

Usage:
    python3 tools/demo_engine.py            # listens on 127.0.0.1:8081
    python3 tools/demo_engine.py 18081      # custom port

Then point a client at it:
    PULSE_CLIENT_HOST=127.0.0.1 PULSE_CLIENT_PORT=8081 ./build_qt/pulseTraderQt
"""

import argparse
import json
import socketserver
import sys
import time

START = time.monotonic()


def status():
    return {
        "version": "0.1.0-demo",
        "uptime_sec": int(time.monotonic() - START),
        "network": "demo",
        "trading_halted": False,
        "active_market": "futures",
        "symbols": ["BTC_USDT", "ETH_USDT"],
        "open_positions": 1,
        "strategies_running": 2,
        "strategies_total": 3,
    }


ACCOUNT = {
    "available": 95.0,
    "futures": {
        "total": 100.0,
        "available_balance": 95.0,
        "unrealised_pnl": 5.0,
        "position_margin": 5.0,
        "order_margin": 0.0,
        "currency": "USDT",
    },
    "spot": {"available": 12.5},
}

POSITIONS = {
    "positions": [
        {
            "position_id": "demo-1",
            "symbol": "BTC_USDT",
            "side": "Buy",
            "quantity": 2,
            "entry_price": 50000.0,
            "unrealized_pnl": 12.5,
            "market_type": "futures",
        }
    ],
    "portfolio": {
        "net_exposure": 100000.0,
        "openPositionCount": 1,
        "total_notional": 100000.0,
        "total_unrealized_pnl": 12.5,
    },
}

ORDERS = {
    "activeOrders": [
        {
            "order_id": "demo-order-1",
            "symbol": "BTC_USDT",
            "side": "Buy",
            "order_type": "limit",
            "price": 49000.0,
            "status": "open",
        }
    ],
    "recentReports": [
        {
            "order_id": "demo-order-2",
            "symbol": "BTC_USDT",
            "status": "filled",
            "fill_price": 50100.0,
            "fill_time_str": "2026-01-01 12:00:00",
        }
    ],
}

SIGNALS = {
    "signals": [
        {
            "source": "momentum_scalper",
            "type": "Buy",
            "confidence": 0.72,
            "reason": "demo engine sample signal",
            "ts_str": "2026-01-01 12:00:00",
        }
    ],
    "aggregate": None,
}


def market(symbol):
    return {
        "symbol": symbol,
        "ticker": {"last": 50123.4, "timestamp": 1794000000000},
        "order_book": {
            "bids": [[50120.0, 1.5], [50118.0, 2.2]],
            "asks": [[50126.0, 2.0], [50129.0, 1.1]],
        },
    }


class Handler(socketserver.StreamRequestHandler):
    """One request per line, one reply per line, no server-initiated messages."""

    def handle(self):
        for raw in self.rfile:
            line = raw.decode("utf-8", "replace").strip()
            if not line:
                continue
            try:
                request = json.loads(line)
            except json.JSONDecodeError:
                continue

            method = request.get("method", "")
            print(f"REQ {method}", flush=True)

            if method == "get_status":
                result = status()
            elif method == "get_account":
                result = ACCOUNT
            elif method == "get_positions":
                result = POSITIONS
            elif method == "get_orders":
                result = ORDERS
            elif method == "get_signals":
                result = SIGNALS
            elif method == "get_market":
                symbol = (request.get("params") or {}).get("symbol", "BTC_USDT")
                result = market(symbol)
            else:
                error = {
                    "jsonrpc": "2.0",
                    "id": request.get("id"),
                    "error": {
                        "code": -32601,
                        "message": f"method not found: {method}",
                        "data": {"code": 9102},
                    },
                }
                self.wfile.write(json.dumps(error).encode() + b"\n")
                self.wfile.flush()
                continue

            reply = {"jsonrpc": "2.0", "id": request.get("id"), "result": result}
            self.wfile.write(json.dumps(reply).encode() + b"\n")
            self.wfile.flush()


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("port", nargs="?", type=int, default=8081,
                        help="TCP port to listen on (default 8081)")
    args = parser.parse_args()

    with Server(("127.0.0.1", args.port), Handler) as server:
        print(f"demo engine listening on 127.0.0.1:{args.port}", flush=True)
        print("sample data only — not a simulator, not connected to an exchange",
              flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
