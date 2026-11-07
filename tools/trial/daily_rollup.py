#!/usr/bin/env python3
"""daily_rollup.py — daily A/B experiment rollup (VPS cron side, stdlib only)

Runs daily on the machine that holds the signal log (the engine VPS):
  1. Summarise the signals table: signals per day/strategy, aggregator emissions
     (cross-checked against the replay), and the growth rate of the signals table
     (a baseline reading of the signal rate during the pilot week)
  2. Replay the equal arm (equal weights) + the ai arm (reads
     data/trial/weights_ai.json; if missing, falls back to equal and marks it)
  3. Write data/trial/<day>/report.md + append to trial.log

Usage (VPS crontab; UTC 16:35 = Beijing 00:35):
  cd ~/pulseTrader && python3 tools/trial/daily_rollup.py \
      --db data/trades.db --symbol BTC_USDT \
      --day-offset 1 2>>data/trial/trial.log >>data/trial/trial.log
Day boundary = UTC calendar day (== Beijing 08:00, the engine's daily-loss reset
boundary). --day-offset 1 = summarise "yesterday" (the whole day); the default 0 =
today so far (a partial day, marked partial).
"""

from __future__ import annotations

import argparse
import json
import os
import sqlite3
import sys
from collections import defaultdict
from datetime import datetime, timedelta, timezone

# Reuse the replayer (same directory)
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from run_replay import run_arm  # noqa: E402

DAY_NS = 86_400 * 1_000_000_000


def bj_day_bounds(offset_days: int):
    """UTC calendar-day boundaries (== the Beijing 08:00 boundary, consistent with
    the engine's daily-loss reset / daily_sync; also aligned with run_replay's
    day_of). Returns (start_ns, end_ns) covering the whole day."""
    now = datetime.now(timezone.utc)
    day = now.date() - timedelta(days=offset_days)
    start = datetime(day.year, day.month, day.day, tzinfo=timezone.utc)
    end = start + timedelta(days=1)
    return int(start.timestamp() * 1e9), int(end.timestamp() * 1e9)


def count_signals(con, symbol: str, start_ns: int, end_ns: int) -> dict:
    q = ("SELECT strategy_id, COUNT(*) FROM signals "
         "WHERE symbol = ? AND ts_ns BETWEEN ? AND ? "
         "AND strategy_id != 'signal_aggregator' GROUP BY strategy_id "
         "ORDER BY COUNT(*) DESC")
    per_strategy = dict(con.execute(q, (symbol, start_ns, end_ns)))
    qa = ("SELECT COUNT(*) FROM signals "
          "WHERE symbol = ? AND strategy_id = 'signal_aggregator' "
          "AND ts_ns BETWEEN ? AND ?")
    row = con.execute(qa, (symbol, start_ns, end_ns)).fetchone()
    return {"per_strategy": per_strategy,
            "total_signals": sum(per_strategy.values()),
            "aggregate_emissions_live": row[0] if row else 0}


def summarize_arm(rts, label: str) -> dict:
    n = len(rts)
    if n == 0:
        return {"arm": label, "round_trips": 0, "net_pnl": 0.0,
                "fees": 0.0, "gross_pnl": 0.0, "win_rate": None}
    net = sum(r.net_pnl for r in rts)
    fees = sum(r.fees for r in rts)
    gross = sum(r.gross_pnl for r in rts)
    wins = sum(1 for r in rts if r.net_pnl > 0)
    return {"arm": label, "round_trips": n, "gross_pnl": round(gross, 4),
            "fees": round(fees, 4), "net_pnl": round(net, 4),
            "win_rate": round(wins / n, 4),
            "expectancy": round(net / n, 6)}


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--db", default="data/trades.db")
    p.add_argument("--symbol", default="BTC_USDT")
    p.add_argument("--day-offset", type=int, default=0,
                   help="0=today so far (partial day, marked); 1+=yesterday (full day)")
    p.add_argument("--notional", type=float, default=160_000.0)
    args = p.parse_args()

    out_root = os.path.join("data", "trial")
    os.makedirs(out_root, exist_ok=True)
    start_ns, end_ns = bj_day_bounds(args.day_offset)
    partial = args.day_offset == 0

    if not os.path.exists(args.db):
        print(f"DB does not exist: {args.db}", file=sys.stderr)
        return 2

    con = sqlite3.connect(f"file:{args.db}?mode=ro", uri=True)
    # Table existence (exit early with a message before deployment / on an early run)
    tbl = con.execute(
        "SELECT name FROM sqlite_master WHERE name='signals'").fetchone()
    if not tbl:
        print("signals table does not exist (engine not deployed with M32) — no data this run",
              file=sys.stderr)
        con.close()
        return 1
    sig_stats = count_signals(con, args.symbol, start_ns, end_ns)
    con.close()

    # equal arm (weight 1.0 for all) + ai arm (weights file; falls back to equal
    # and is marked when missing)
    weights_path = os.path.join(out_root, "weights_ai.json")
    ai_weights = None
    if os.path.exists(weights_path):
        with open(weights_path, encoding="utf-8") as f:
            ai_weights = json.load(f)

    # Replay (limited to the time window; ticks cover that whole day); the
    # threshold / cooldown / exit parameters match the paper_engine defaults
    # (trading.toml [strategy]/[risk])
    cfg = {"threshold": 0.6, "cooldown_s": 30,
           "sl_trailing_pct": 0.005, "max_hold_s": 300,
           "tp_targets": [0.005, 0.01, 0.02],
           "tp_fractions": [1 / 3, 1 / 3, 1 / 3],
           "maker_timeout_s": 5.0}
    agg_eq, trader_eq, _ = run_arm(args.db, args.symbol, {}, cfg,
                                   since_ns=start_ns, until_ns=end_ns,
                                   notional_usdt=args.notional)
    agg_ai, trader_ai, _ = run_arm(args.db, args.symbol, ai_weights or {},
                                   cfg, since_ns=start_ns, until_ns=end_ns,
                                   notional_usdt=args.notional)

    day_str = datetime.fromtimestamp(start_ns / 1e9,
                                     tz=timezone.utc).strftime("%Y-%m-%d")
    out = {
        "day": day_str, "partial": partial,
        "signals": sig_stats,
        "emissions_replayed_equal": agg_eq.emissions,
        "emissions_replayed_ai": agg_ai.emissions,
        "ai_weights_used": ai_weights is not None,
        "arms": [summarize_arm(trader_eq.round_trips, "equal"),
                 summarize_arm(trader_ai.round_trips, "ai")],
    }
    report_path = os.path.join(out_root, f"{day_str}.json")
    with open(report_path, "w") as f:
        json.dump(out, f, indent=2, ensure_ascii=False)

    # Console summary (captured into trial.log by cron)
    print(f"[rollup] {day_str}" + ("(partial)" if partial else "")
          + f" signals {out['signals']['total_signals']} | "
          + f"emitted replay equal {out['emissions_replayed_equal']} / "
          + f"ai {out['emissions_replayed_ai']} | live aggregate "
          + f"{out['signals']['aggregate_emissions_live']} | AI weights used: "
          + str(out['ai_weights_used']))
    for arm in out["arms"]:
        if arm["round_trips"]:
            print(f"  {arm['arm']}: {arm['round_trips']} round trips | "
                  f"net {arm['net_pnl']:+.2f} | fees {arm['fees']:.2f} | "
                  f"win rate {arm['win_rate']:.1%} | expectancy {arm['expectancy']:+.4f}")
        else:
            print(f"  {arm['arm']}: no round trips")
    print(f"  → {report_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
