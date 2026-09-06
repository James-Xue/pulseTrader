#!/usr/bin/env python3
"""Deep-pull 1h/4h/1d Gate klines into data/klines/*.csv (public REST, no API key).

Gate's candle retention is ~10000 points per series, sliding with time:
    - 1m  ≈ 6.9 days   (why tools/fetch_klines.py / kline-store accumulate daily)
    - 5m  ≈ 35 days    (futures) / spot slightly shallower (~9900 points)
    - 15m ≈ 104 days
    - 30m ≈ 208 days
    - 1h  ≈ 416 days
    - 4h  ≈ 4.6 years
    - 1d  ≈ 27 years   (full history since listing)
Beyond the boundary the API answers HTTP 400 "Candlestick too long ago.
Maximum 10000 points recently are allowed" (probed 2026-09-06). So higher
timeframes need no daily accumulation — every run re-pulls the whole window.

Strategy per series (lossless): walk newest -> oldest in step-size windows
until a request 400s; then binary-search the exact oldest bar the server
still serves, and pull the final sliver.

Usage:
    python3 tools/fetch_tf_klines.py [--symbols BTC_USDT,ETH_USDT]
        [--market both|futures|spot] [--intervals 1h,4h,1d]
        [--dir data/klines] [--workers N] [--resume]

Writes <sym>_<futures|spot>_<interval>.csv with the legacy *_1m.csv layout:
ts,open,high,low,close,volume (ts = open time, sec UTC). Writes go through a
.part temp file + atomic rename, so an interrupted run never leaves a torn
file; --resume skips series whose target file already exists (complete).
"""
import argparse
import concurrent.futures
import datetime
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

BASE = "https://api.gateio.ws/api/v4"
IV_SEC = {"1m": 60, "5m": 300, "15m": 900, "30m": 1800, "1h": 3600, "4h": 14400, "1d": 86400}
STEP_BARS = {"futures": 2000, "spot": 900}  # bars per request (server caps 2000/1000)
MAX_DEPTH_BARS = 10200                      # walk never needs to go deeper than this
HEADER = "ts,open,high,low,close,volume\n"
_log_lock = None  # set in main(); serializes progress lines under concurrency


def get(path: str, params: dict):
    url = BASE + path + "?" + urllib.parse.urlencode(params)
    last = None
    for attempt in range(3):
        try:
            with urllib.request.urlopen(url, timeout=30) as r:
                return json.load(r)
        except urllib.error.HTTPError as e:
            if e.code == 400:  # "too long ago" (or bad params) → distinguish from outage
                return None
            last = e
        except Exception as e:
            last = e
        time.sleep(1.2 * (attempt + 1))
    print(f"  WARN: fetch failed after retries ({last})", file=sys.stderr)
    return None


def parse_bar(row, market: str):
    """(ts, open, high, low, close, volume) from either response layout."""
    if market == "futures":  # {"t","o","h","l","c","v"}
        return int(row["t"]), row["o"], row["h"], row["l"], row["c"], row["v"]
    # spot: [ts, quote_vol, close, high, low, open, base_vol, closed]
    return int(row[0]), row[5], row[3], row[4], row[2], row[6]


def valid_at(market: str, pair: str, interval: str, ts: int):
    """True if the server still serves a bar opening at ts (single-bar probe)."""
    iv = IV_SEC[interval]
    path = "/futures/usdt/candlesticks" if market == "futures" else "/spot/candlesticks"
    key = {"contract": pair} if market == "futures" else {"currency_pair": pair}
    d = get(path, {**key, "interval": interval, "from": ts, "to": ts + iv - 1})
    return bool(d)


def pull(market: str, pair: str, interval: str, out: Path):
    iv = IV_SEC[interval]
    now = int(time.time())
    path = "/futures/usdt/candlesticks" if market == "futures" else "/spot/candlesticks"
    key = {"contract": pair} if market == "futures" else {"currency_pair": pair}
    step = STEP_BARS[market] * iv

    rows: dict = {}
    to = now - 1
    limit_bar = now - MAX_DEPTH_BARS * iv  # sanity bound; walk must stop before this
    while to >= limit_bar:
        lo = to - step + 1
        d = get(path, {**key, "interval": interval, "from": lo, "to": to})
        if d is None:
            break  # [lo, to] crosses the depth boundary → hunt the exact oldest bar
        new = 0
        for row in d:
            ts, o, h, l, c, v = parse_bar(row, market)
            if ts not in rows:
                rows[ts] = (o, h, l, c, v)
                new += 1
        if new == 0 and not d:
            break  # empty answer above the boundary is not expected; stop anyway
        to = lo - 1
        time.sleep(0.15)

    if rows and not valid_at(market, pair, interval, min(rows)):
        # The crossing window 400'd, so the true floor sits inside [last_bad_lo, min(rows)].
        # Binary-search the smallest bar ts the server still serves.
        bad_lo, ok_lo = to + 1, min(rows)  # to+1 = bottom of the window that 400'd
        while ok_lo - bad_lo > iv:
            mid = bad_lo + (ok_lo - bad_lo) // 2 // iv * iv  # align mid to a bar
            if valid_at(market, pair, interval, mid):
                ok_lo = mid
            else:
                bad_lo = mid + iv
            time.sleep(0.15)
        d = get(path, {**key, "interval": interval, "from": ok_lo, "to": min(rows) - 1})
        if d:
            for row in d:
                ts, o, h, l, c, v = parse_bar(row, market)
                rows[ts] = (o, h, l, c, v)

    part = out.with_suffix(".csv.part")
    with part.open("w") as f:
        f.write(HEADER)
        for ts in sorted(rows):
            o, h, l, c, v = rows[ts]
            f.write(f"{ts},{o},{h},{l},{c},{v}\n")
    os.replace(part, out)  # atomic — a reader/resumer never sees a torn file
    return rows


def one_series(interval: str, market: str, sym: str, out_dir: Path, resume: bool):
    lo_sym = sym[:-5].lower() if sym.endswith("_USDT") else sym.lower()
    fname = out_dir / f"{lo_sym}_{market}_{interval}.csv"
    if resume and fname.exists() and fname.stat().st_size > len(HEADER):
        with _log_lock:
            print(f"== {sym} {market} {interval} -> {fname}  [skip: exists]", flush=True)
        return
    if not resume:
        fname.unlink(missing_ok=True)  # never overwrite a stale file without --resume
    with _log_lock:
        print(f"== {sym} {market} {interval} -> {fname}", flush=True)
    rows = pull(market, sym, interval, fname)
    with _log_lock:
        if rows:
            print(f"   {len(rows)} bars, "
                  f"{datetime.datetime.fromtimestamp(min(rows), datetime.UTC):%Y-%m-%d %H:%M}Z .. "
                  f"{datetime.datetime.fromtimestamp(max(rows), datetime.UTC):%Y-%m-%d %H:%M}Z", flush=True)
        else:
            print("   no data", file=sys.stderr)


def main():
    global _log_lock
    ap = argparse.ArgumentParser()
    ap.add_argument("--symbols", default="BTC_USDT")
    ap.add_argument("--market", default="both", choices=["both", "futures", "spot"])
    ap.add_argument("--intervals", default="1h,4h,1d")
    ap.add_argument("--dir", default="data/klines")
    ap.add_argument("--workers", type=int, default=1)
    ap.add_argument("--resume", action="store_true")
    args = ap.parse_args()

    out_dir = Path(args.dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    _log_lock = __import__("threading").Lock()
    symbols = [s.strip() for s in args.symbols.split(",") if s.strip()]
    markets = ["futures", "spot"] if args.market == "both" else [args.market]
    intervals = [s.strip() for s in args.intervals.split(",") if s.strip()]
    jobs = [(iv, m, sym) for iv in intervals for m in markets for sym in symbols]
    print(f"series: {len(jobs)} (workers={args.workers}, resume={args.resume})", flush=True)

    done = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as ex:
        futs = [ex.submit(one_series, iv, m, s, out_dir, args.resume) for iv, m, s in jobs]
        for fut in concurrent.futures.as_completed(futs):
            fut.result()  # surface exceptions
            done += 1
            if done % 10 == 0:
                with _log_lock:
                    print(f"--- {done}/{len(jobs)} series done ---", flush=True)
    print(f"all {done}/{len(jobs)} series done", flush=True)


if __name__ == "__main__":
    sys.exit(main())
