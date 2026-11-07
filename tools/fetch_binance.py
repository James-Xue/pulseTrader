#!/usr/bin/env python3
"""Deep-pull FULL-LISTING Binance kline history into data/binance/*.csv.

Unlike Gate (whose public REST keeps only ~10000 points ≈ 6.9 days of 1m),
Binance serves 1m candles back to each symbol's listing (BTC perp 2019-09,
BTC spot 2017-08, verified 2026-09-06). This mirrors a catalog of symbols to
repair research windows and give multi-year context.

Venues (both public, no API key):
    - perp:  fapi.binance.com /fapi/v1/klines   (USDⓈ-M perpetual ≈ closest
             public proxy for Gate's futures feed — small cross-venue basis)
    - spot:  api.binance.com  /api/v3/klines

Response row: [openTime_ms, open, high, low, close, volume, closeTime_ms,
quoteVol, trades, takerBuyBase, takerBuyQuote, ignore]. We write the Gate-CSV
layout: ts(sec),open,high,low,close,volume.

Paging: walk newest -> oldest in 1000-bar windows; empty window = reached the
listing start. Atomic .part write + --resume (skip complete files) + --workers
for concurrency. A 400/418 (bad symbol / IP ban) surfaces as a warning and the
series is skipped — symbol probes happen first.

Usage:
    python3 tools/fetch_binance.py [--symbols BTC_USDT,ETH_USDT]
        [--market both|perp|spot] [--intervals 1m,5m,15m,30m,1h,4h,1d]
        [--dir data/binance] [--workers 2] [--resume] [--only NAME] [--no-probe]
"""
import argparse
import concurrent.futures
import datetime
import json
import os
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

BASE = {"perp": "https://fapi.binance.com/fapi/v1/klines",
        "spot": "https://api.binance.com/api/v3/klines"}
FUND_URL = "https://fapi.binance.com/fapi/v1/fundingRate"
PAGE = {"perp": 1500, "spot": 1000}  # Binance max rows per request per venue
IV_MS = {"1m": 60_000, "5m": 300_000, "15m": 900_000, "30m": 1_800_000,
         "1h": 3_600_000, "4h": 14_400_000, "1d": 86_400_000}
HEADER = "ts,open,high,low,close,volume\n"
_log_lock = None
_rate = 5.0                  # global request cap per second (Binance IP bans on bursts)
INVALID = object()           # HTTP 400: bad symbol / bad params (permanent)
EXHAUSTED = object()         # network retries spent (transient — must NOT write data)
_throttle_lock = threading.Lock()
_last_request = 0.0


def throttle():
    """Global token pacing: at most --rate requests/second across all workers.

    Binance futures API budgets ~2400 weight/min per IP and returns 429 the
    moment a burst overshoots (measured 2026-09-06: ~2.5/s and ~5/s are clean,
    8+/s bursts get banned for tens of seconds). Workers + sleep-per-worker do
    NOT bound aggregate rate, so the cap lives here.
    """
    global _last_request
    with _throttle_lock:
        now = time.monotonic()
        wait = 1.0 / _rate - (now - _last_request)
        if wait > 0:
            time.sleep(wait)
        _last_request = time.monotonic()


def get(market: str, params: dict):
    return get_url(BASE[market], params)


def get_url(url: str, params: dict):
    throttle()
    url = url + "?" + urllib.parse.urlencode(params)
    last = None
    for attempt in range(4):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "pulseTrader/1.0"})
            with urllib.request.urlopen(req, timeout=30) as r:
                return json.load(r)
        except urllib.error.HTTPError as e:
            body = e.read().decode("utf-8", "replace")[:120]
            if e.code == 400:
                return INVALID  # bad symbol / bad params (permanent)
            if e.code in (418, 429):
                retry = int(e.headers.get("Retry-After", "10"))
                print(f"  rate-limited ({e.code}): pause {retry}s", file=sys.stderr)
                time.sleep(retry + 1)
                last = e
                continue
            last = f"HTTP {e.code} {body}"
        except Exception as e:
            last = e
        time.sleep(1.2 * (attempt + 1))
    print(f"  WARN: fetch failed after retries ({last})", file=sys.stderr)
    return EXHAUSTED


def api_symbol(sym: str) -> str:
    """Gate-style BTC_USDT -> Binance-style BTCUSDT (no underscore)."""
    return sym.replace("_", "")


def probe(market: str, sym: str) -> bool:
    """True if the symbol exists on this venue (1-bar probe at the listing start)."""
    d = get(market, {"symbol": api_symbol(sym), "interval": "1d", "startTime": 0, "limit": 1})
    return d is not INVALID and d is not EXHAUSTED and bool(d)


def pull(market: str, sym: str, interval: str, out: Path):
    iv = IV_MS[interval]
    rows: dict = {}
    aborted = False
    to = int(time.time() * 1000)
    empty_windows = 0
    while True:
        d = get(market, {"symbol": api_symbol(sym), "interval": interval,
                         "endTime": to, "limit": PAGE[market]})
        if d is INVALID:
            print(f"  stop: invalid/missing symbol or rejected request", file=sys.stderr)
            break
        if d is EXHAUSTED:
            print(f"  abort (transient network): nothing written, will retry on --resume",
                  file=sys.stderr)
            aborted = True
            break
        if not d:
            empty_windows += 1
            if empty_windows >= 2 or to <= iv:
                break  # listing start reached (1 empty window is normal at the edge)
            to -= iv  # step one window further back and retry
            continue
        new = 0
        for r in d:
            ts = int(r[0]) // 1000  # openTime ms -> sec, same layout as Gate CSVs
            if ts not in rows:
                rows[ts] = (r[1], r[2], r[3], r[4], r[5])
                new += 1
        if new == 0 and to <= 0:
            break
        oldest = min(int(r[0]) for r in d)
        to = oldest - 1
        if empty_windows:
            empty_windows = 0
        time.sleep(0.10)

    if aborted:
        return None  # transient failure — no file (even partial) so --resume retries
    part = out.with_suffix(".csv.part")
    with part.open("w") as f:
        f.write(HEADER)
        for ts in sorted(rows):
            o, h, l, c, v = rows[ts]
            f.write(f"{ts},{o},{h},{l},{c},{v}\n")
    os.replace(part, out)
    return rows


def one_series(interval: str, market: str, sym: str, out_dir: Path, resume: bool):
    lo = sym[:-5].lower() if sym.endswith("_USDT") else sym.lower()
    fname = out_dir / f"{lo}_{market}_{interval}.csv"
    if resume and fname.exists() and fname.stat().st_size > len(HEADER):
        with _log_lock:
            print(f"== {sym} {market} {interval} -> {fname}  [skip: exists]", flush=True)
        return
    if not resume:
        fname.unlink(missing_ok=True)
    with _log_lock:
        print(f"== {sym} {market} {interval} -> {fname}", flush=True)
    rows = pull(market, sym, interval, fname)
    with _log_lock:
        if rows:
            print(f"   {len(rows)} bars, "
                  f"{datetime.datetime.fromtimestamp(min(rows), datetime.UTC):%Y-%m-%d %H:%M}Z .. "
                  f"{datetime.datetime.fromtimestamp(max(rows), datetime.UTC):%Y-%m-%d %H:%M}Z", flush=True)
        else:
            print("   no data (symbol missing?)", file=sys.stderr)


def pull_funding(sym: str, out: Path):
    """Full funding-rate history for one perp (8h cadence, back to listing)."""
    rows: dict = {}
    start = 0
    while True:
        d = get_url(FUND_URL, {"symbol": api_symbol(sym), "startTime": start, "limit": 1000})
        if d is INVALID:
            print(f"  stop: invalid symbol for funding", file=sys.stderr)
            return None
        if d is EXHAUSTED:
            print(f"  abort (transient network): funding nothing written",
                  file=sys.stderr)
            return None
        if not d:
            break
        new = 0
        for r in d:
            ts = int(r["fundingTime"]) // 1000
            if ts not in rows:
                rows[ts] = r["fundingRate"]
                new += 1
        if new == 0 or int(d[-1]["fundingTime"]) <= start:
            break
        start = int(d[-1]["fundingTime"]) + 1
    part = out.with_suffix(".csv.part")
    with part.open("w") as f:
        f.write("ts,funding_rate\n")
        for ts in sorted(rows):
            f.write(f"{ts},{rows[ts]}\n")
    os.replace(part, out)
    return rows


def run_funding(symbols, out_dir: Path, resume: bool):
    fdir = out_dir / "funding"
    fdir.mkdir(parents=True, exist_ok=True)
    for sym in symbols:
        lo = sym[:-5].lower() if sym.endswith("_USDT") else sym.lower()
        fname = fdir / f"{lo}_perp_funding.csv"
        if resume and fname.exists() and fname.stat().st_size > 20:
            with _log_lock:
                print(f"== {sym} funding -> {fname}  [skip: exists]", flush=True)
            continue
        with _log_lock:
            print(f"== {sym} funding -> {fname}", flush=True)
        rows = pull_funding(sym, fname)
        with _log_lock:
            if rows:
                import datetime as _dt
                print(f"   {len(rows)} rates, "
                      f"{_dt.datetime.fromtimestamp(min(rows), _dt.UTC):%Y-%m-%d}Z .. "
                      f"{_dt.datetime.fromtimestamp(max(rows), _dt.UTC):%Y-%m-%d}Z", flush=True)
            else:
                print("   no data", file=sys.stderr)


def main():
    global _log_lock, _rate
    ap = argparse.ArgumentParser()
    ap.add_argument("--symbols", default="BTC_USDT")
    ap.add_argument("--market", default="both", choices=["both", "perp", "spot"])
    ap.add_argument("--intervals", default="1m,5m,15m,30m,1h,4h,1d")
    ap.add_argument("--dir", default="data/binance")
    ap.add_argument("--workers", type=int, default=2)
    ap.add_argument("--rate", type=float, default=5.0,
                    help="global request cap per second (Binance IP 429 guard)")
    ap.add_argument("--resume", action="store_true")
    ap.add_argument("--no-probe", action="store_true")
    ap.add_argument("--funding", action="store_true",
                    help="pull perp funding-rate full history into <dir>/funding/")
    args = ap.parse_args()
    _rate = args.rate

    out_dir = Path(args.dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    _log_lock = threading.Lock()
    symbols = [s.strip() for s in args.symbols.split(",") if s.strip()]
    if args.funding:
        _log_lock = threading.Lock()
        run_funding(symbols, out_dir, args.resume)
        return 0
    markets = ["perp", "spot"] if args.market == "both" else [args.market]
    intervals = [s.strip() for s in args.intervals.split(",") if s.strip()]

    jobs = []
    for interval in intervals:
        for market in markets:
            for sym in symbols:
                lo = sym[:-5].lower() if sym.endswith("_USDT") else sym.lower()
                fname = out_dir / f"{lo}_{market}_{interval}.csv"
                if args.resume and fname.exists() and fname.stat().st_size > len(HEADER):
                    continue  # not even queued
                if not args.no_probe and not probe(market, sym):
                    with _log_lock:
                        print(f"-- {sym} not on Binance {market}; skipped", flush=True)
                    continue
                jobs.append((interval, market, sym))
    print(f"series: {len(jobs)} (workers={args.workers}, resume={args.resume})", flush=True)

    done = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as ex:
        futs = [ex.submit(one_series, iv, m, s, out_dir, args.resume) for iv, m, s in jobs]
        for fut in concurrent.futures.as_completed(futs):
            fut.result()
            done += 1
            if done % 10 == 0:
                with _log_lock:
                    print(f"--- {done}/{len(jobs)} series done ---", flush=True)
    print(f"all {done}/{len(jobs)} series done", flush=True)


if __name__ == "__main__":
    sys.exit(main())
