#!/usr/bin/env python3
"""Bulk-download recent Gate klines (public REST, no API key).

Fetches 1m klines for BTC/ETH (futures + spot) and XAUUSD (CFD) into
data/klines/*_1m.csv.

Usage:
    python3 tools/fetch_klines.py [hours]    # default: last 48h

Endpoints & limits (verified 2026-08-23):
    - futures  /api/v4/futures/usdt/candlesticks  limit<=2000; page via from/to (mutually exclusive with limit)
    - spot     /api/v4/spot/candlesticks          limit<=1000; page via from/to
    - gold     /api/v4/tradfi/symbols/XAUUSD/klines  limit<=500 (~8.3h of 1m); from/to ignored,
                newest bar lags ~33h during weekend close; use kline_type=5m/15m/1d for deeper history
"""
import json
import sys
import time
import urllib.parse
import urllib.request
from pathlib import Path

BASE = "https://api.gateio.ws/api/v4"
NOW = int(time.time())
HOURS = float(sys.argv[1]) if len(sys.argv) > 1 else 48.0
STEP = 1000 * 60     # futures window: 1000 bars per request (cap 2000)
STEP_SPOT = 900 * 60  # spot window: 900 bars per request (cap 1000, wider errors)


def get(path: str, params: dict):
    url = BASE + path + "?" + urllib.parse.urlencode(params)
    for _ in range(3):  # retry transient errors
        try:
            with urllib.request.urlopen(url, timeout=30) as r:
                return json.load(r)
        except Exception:
            time.sleep(1)
    raise SystemExit(f"fetch failed: {url}")


def pull_futures(contract: str):
    out, to = [], NOW
    fr = NOW - int(HOURS * 3600)
    while to > fr:
        lo = max(fr, to - STEP)  # walk newest -> oldest, max 1000 bars per window
        d = get("/futures/usdt/candlesticks", {"contract": contract, "interval": "1m", "from": lo, "to": to})
        if not d:
            break
        out += [(x["t"], x["o"], x["h"], x["l"], x["c"], x["v"]) for x in d]
        to = lo
        time.sleep(0.3)
    return out


def pull_spot(pair: str):
    out, to = [], NOW
    fr = NOW - int(HOURS * 3600)
    while to > fr:
        lo = max(fr, to - STEP_SPOT)  # spot cap is 1000 bars; 900 leaves margin
        d = get("/spot/candlesticks", {"currency_pair": pair, "interval": "1m", "from": lo, "to": to})
        if not d:
            break
        # spot field order: [ts, quote_vol, close, high, low, open, base_vol, closed]
        out += [(int(x[0]), x[5], x[3], x[4], x[2], x[6]) for x in d]
        to = lo
        time.sleep(0.3)
    return out


def pull_xau():
    d = get("/tradfi/symbols/XAUUSD/klines", {"kline_type": "1m", "limit": 500})
    # no volume field; CFD field order: {t,o,h,l,c}
    return [(x["t"], x["o"], x["h"], x["l"], x["c"], "") for x in d["data"]["list"]]


TARGETS = {
    "btc_futures": pull_futures("BTC_USDT"),
    "eth_futures": pull_futures("ETH_USDT"),
    "btc_spot":    pull_spot("BTC_USDT"),
    "eth_spot":    pull_spot("ETH_USDT"),
    "xau_cfd":     pull_xau(),
}

outdir = Path(__file__).resolve().parent.parent / "data" / "klines"
outdir.mkdir(parents=True, exist_ok=True)
for name, rows in TARGETS.items():
    rows = sorted(set(rows))  # dedupe + sort by time
    with open(outdir / f"{name}_1m.csv", "w") as f:
        f.write("ts,open,high,low,close,volume\n")
        f.writelines(f"{t},{o},{h},{l},{c},{v}\n" for t, o, h, l, c, v in rows)
    print(f"{name}: {len(rows)} bars -> {outdir / (name + '_1m.csv')}")
