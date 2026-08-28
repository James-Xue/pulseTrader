#!/usr/bin/env python3
"""Bulk-download recent Gate klines (public REST, no API key).

Fetches 1m klines for every symbol the engine follows into data/klines/*_1m.csv:
    - futures: BTC/ETH/SNDK/UNITREE_USDT  (~10000 bars ≈ 6.9 days; 9995-bar safe window)
    - spot:    BTC/ETH_USDT               (9990-bar safe window; SNDK/UNITREE have no spot market)
    - tradFi:  XAUUSD/XAGUSD              (limit 500 ≈ 8.3h of 1m — endpoint hard cap)

Usage:
    python3 tools/fetch_klines.py [hours]    # default: 168 (= as much as each endpoint allows)

Endpoints & limits (verified 2026-08-23, see codex-brain memory-details/reference/gate-kline-api.md):
    - futures  /api/v4/futures/usdt/candlesticks  limit<=2000; page via from/to (mutually exclusive with limit)
    - spot     /api/v4/spot/candlesticks          limit<=1000; page via from/to
    - tradFi   /api/v4/tradfi/symbols/{SYM}/klines  limit<=500 (~8.3h of 1m); from/to ignored;
                must pass kline_type=1m or INVALID_ARGUMENT; newest bar lags ~33s;
                deeper history needs kline_type=5m/15m/1d
"""
import json
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

BASE = "https://api.gateio.ws/api/v4"
NOW = int(time.time())
HOURS = float(sys.argv[1]) if len(sys.argv) > 1 else 168.0

FUT_MAX_BARS = 9995    # server cap "Maximum 10000 points recently"; margin for the moving boundary
SPOT_MAX_BARS = 9990   # spot 1m depth is slightly shallower than futures
STEP_FUT = 1000 * 60   # futures window: 1000 bars per request (cap 2000)
STEP_SPOT = 900 * 60   # spot window: 900 bars per request (cap 1000, wider errors)
CFD_LIMIT = 500        # tradFi hard cap (~8.3h of 1m)


def get(path: str, params: dict):
    url = BASE + path + "?" + urllib.parse.urlencode(params)
    last = None
    for attempt in range(3):  # retry transient errors
        try:
            with urllib.request.urlopen(url, timeout=30) as r:
                return json.load(r)
        except urllib.error.HTTPError as e:
            if e.code in (400, 404):  # out of depth / unknown pair: stop this pull, keep what we have
                print(f"  stop: HTTP {e.code} {e.reason} (depth boundary?)", file=sys.stderr)
                return None
            last = e
        except Exception as e:
            last = e
        time.sleep(1.5 * (attempt + 1))
    print(f"  WARN: fetch failed after retries ({last})", file=sys.stderr)
    return None


def pull_futures(contract: str):
    out, to = [], NOW
    fr = max(NOW - int(HOURS * 3600), NOW - FUT_MAX_BARS * 60)  # walk newest -> oldest
    while to > fr:
        lo = max(fr, to - STEP_FUT)
        d = get("/futures/usdt/candlesticks", {"contract": contract, "interval": "1m", "from": lo, "to": to})
        if not d:
            break
        out += [(x["t"], x["o"], x["h"], x["l"], x["c"], x["v"]) for x in d]
        to = lo
        time.sleep(0.3)
    return out


def pull_spot(pair: str):
    out, to = [], NOW
    fr = max(NOW - int(HOURS * 3600), NOW - SPOT_MAX_BARS * 60)
    while to > fr:
        lo = max(fr, to - STEP_SPOT)
        d = get("/spot/candlesticks", {"currency_pair": pair, "interval": "1m", "from": lo, "to": to})
        if not d:
            break
        # spot field order: [ts, quote_vol, close, high, low, open, base_vol, closed]
        out += [(int(x[0]), x[5], x[3], x[4], x[2], x[6]) for x in d]
        to = lo
        time.sleep(0.3)
    return out


def pull_cfd(symbol: str):
    d = get(f"/tradfi/symbols/{symbol}/klines", {"kline_type": "1m", "limit": CFD_LIMIT})
    if not d:
        return []
    # no volume field; CFD field order: {t,o,h,l,c}
    return [(x["t"], x["o"], x["h"], x["l"], x["c"], "") for x in d["data"]["list"]]


TARGETS = {
    "btc_futures":    ("futures", "BTC_USDT"),
    "eth_futures":    ("futures", "ETH_USDT"),
    "sndk_futures":   ("futures", "SNDK_USDT"),
    "unitree_futures": ("futures", "UNITREE_USDT"),
    "btc_spot":       ("spot", "BTC_USDT"),
    "eth_spot":       ("spot", "ETH_USDT"),
    "xau_cfd":        ("cfd", "XAUUSD"),
    "xag_cfd":        ("cfd", "XAGUSD"),
}

PULLERS = {"futures": pull_futures, "spot": pull_spot, "cfd": pull_cfd}


def fmt(ts):
    return time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(ts + 8 * 3600))  # 北京


outdir = Path(__file__).resolve().parent.parent / "data" / "klines"
outdir.mkdir(parents=True, exist_ok=True)
for name, (kind, symbol) in TARGETS.items():
    rows = sorted(set(PULLERS[kind](symbol)))  # dedupe + sort by time
    if not rows:
        print(f"{name}: 0 bars (pull failed) — skipped")
        continue
    with open(outdir / f"{name}_1m.csv", "w") as f:
        f.write("ts,open,high,low,close,volume\n")
        f.writelines(f"{t},{o},{h},{l},{c},{v}\n" for t, o, h, l, c, v in rows)
    print(f"{name}: {len(rows):5d} bars  {fmt(rows[0][0])} ~ {fmt(rows[-1][0])} (北京)  ->  {outdir / (name + '_1m.csv')}")

print("\n注:最后一根可能为未收盘(forming)K线;合约/现货 ~10000 根≈6.9 天,黄金/白银 REST 上限仅 500 根(≈8.3h)")