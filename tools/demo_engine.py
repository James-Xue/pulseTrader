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
    "spot": {"available": 12.5, "currency": "USDT", "total": 12.5},
    # One row per non-zero spot currency. `value_usdt` is null when the engine
    # has no cached ticker; the client renders a dash instead of zero.
    "spot_balances": [
        {
            "currency": "USDT", "available": 12.5, "locked": 0.0,
            "total": 12.5, "price_usdt": 1.0, "value_usdt": 12.5,
        },
        {
            "currency": "XAUT", "available": 0.0012, "locked": 0.0,
            "total": 0.0012, "price_usdt": 2400.0, "value_usdt": 2.88,
        },
        {
            "currency": "SLVON", "available": 0.0055, "locked": 0.0,
            "total": 0.0055, "price_usdt": 30.0, "value_usdt": 0.165,
        },
        {
            "currency": "LSK", "available": 1.5, "locked": 0.0,
            "total": 1.5, "price_usdt": None, "value_usdt": None,
        },
    ],
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

# Strategy instances. Two gold CFD instances are visible on the Gold Traders
# page; the silver and BTC rows exist to prove the exact-symbol filter.
STRATEGIES = [
    {
        "id": "momentum_scalper_XAUUSD",
        "name": "MomentumScalper",
        "symbol": "XAUUSD",
        "enabled": True,
        "running": True,
        "paused": False,
        "auto_trade": False,
        "poll_interval_ms": 500,
    },
    {
        "id": "mean_reversion_scalper_XAUUSD",
        "name": "MeanReversionScalper",
        "symbol": "XAUUSD",
        "enabled": True,
        "running": True,
        "paused": True,
        "auto_trade": False,
        "poll_interval_ms": 500,
    },
    {
        "id": "momentum_scalper_XAGUSD",
        "name": "MomentumScalper",
        "symbol": "XAGUSD",
        "enabled": True,
        "running": True,
        "paused": True,
        "auto_trade": False,
        "poll_interval_ms": 500,
    },
    {
        "id": "momentum_scalper_BTC_USDT",
        "name": "MomentumScalper",
        "symbol": "BTC_USDT",
        "enabled": True,
        "running": True,
        "paused": False,
        "auto_trade": True,
        "poll_interval_ms": 500,
    },
]

STRATEGY_PARAMS = {
    "momentum_scalper_XAUUSD": {
        "order_quantity": 0.01,
        "min_confidence": 0.6,
        "ema_fast_period": 9.0,
        "ema_slow_period": 21.0,
        "bb_period": 20.0,
        "bb_std_dev": 2.0,
        "ob_imbalance_threshold": 0.65,
        "ob_depth": 5.0,
        "supertrend_period": 10.0,
        "supertrend_multiplier": 3.0,
        "cooldown_seconds": 60.0,
        "stop_loss_pct": 0.5,
        "take_profit_pct": 1.0,
        "auto_trade": 0.0,
    },
    "mean_reversion_scalper_XAUUSD": {
        "order_quantity": 0.01,
        "min_confidence": 0.55,
        "ema_fast_period": 7.0,
        "ema_slow_period": 30.0,
        "bb_period": 20.0,
        "bb_std_dev": 2.2,
        "ob_imbalance_threshold": 0.6,
        "ob_depth": 5.0,
        "supertrend_period": 10.0,
        "supertrend_multiplier": 3.0,
        "cooldown_seconds": 90.0,
        "stop_loss_pct": 0.4,
        "take_profit_pct": 0.8,
        "auto_trade": 0.0,
    },
}

RISK = {
    "trading_halted": False,
    "haltReason": 0,
    "dailyDrawdown": 0.012,
    "maxDrawdown": 0.034,
    "rate_limiter_tokens": 8.0,
    "rate_limiter_exhausted": False,
    "portfolio": {
        "openPositionCount": 1,
        "total_notional": 4392.0,
        "total_unrealized_pnl": 12.5,
        "net_exposure": 4392.0,
    },
    "openPositionCount": 1,
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
            elif method == "list_strategies":
                result = STRATEGIES
            elif method == "get_strategy_params":
                strategy_id = (request.get("params") or {}).get("strategy_id", "")
                result = STRATEGY_PARAMS.get(strategy_id)
                if result is None:
                    error = {
                        "jsonrpc": "2.0",
                        "id": request.get("id"),
                        "error": {
                            "code": -32603,
                            "message": f"strategy not found: {strategy_id}",
                            "data": {"code": 9101},
                        },
                    }
                    self.wfile.write(json.dumps(error).encode() + b"\n")
                    self.wfile.flush()
                    continue
            elif method == "get_risk":
                result = RISK
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
