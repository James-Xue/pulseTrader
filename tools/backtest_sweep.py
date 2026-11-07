#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""backtest_sweep.py — Parameter sweep over the C++ backtest engine (M33-B)

Spawns the existing `pulsetrader backtest` binary once per parameter combo
(with --json export), parses each report, and appends one CSV row per run.
The engine's sqlite write-back warms the data on the first run, so later
parallel runs serve from local kline_bars with zero REST calls (verify via
the rows_api column).

Sweep param grammar (per --param KEY=SPEC, repeatable):
    KEY=5|9|21           discrete values
    KEY=34..100:11       range from..to:step (step defaults to 1)
    KEY=2                single (constant across runs)

Keys are passed through as --param KEY=VALUE, so atomic hot params
(ema_fast_period, bb_std_dev, ...) and custom_params-channel keys
(res_ema_p1..p5, eth_atr_step, ...) work identically — the engine classifies.

Every other flag is passed through verbatim to the backtest CLI
(--quantity, --fee-rate, --market, --from, --to, --no-api, --no-cache,
--db, --config, ...).

Sweep param sets are EXPERIMENT artifacts, not registered strategies — they
need no docs/strategies/ rule document. Promote a winning set to a live
strategy via config custom_params only after it survives real scrutiny.

Examples:
    python3 tools/backtest_sweep.py --strategy momentum_scalper \\
        --symbol DOGE_USDT --from 2026-09-01 --to 2026-09-06 \\
        --param ema_fast_period=5|9|21 --param min_confidence=0.5|0.7 \\
        --quantity 20 --max-combos 100
    python3 tools/backtest_sweep.py --strategy momentum_scalper \\
        --symbol SNDK_USDT --from 2026-08-20 --to 2026-09-03 --no-api \\
        --param ema_slow_period=34..100:11 --out /tmp/sweep.csv
"""

import argparse
import concurrent.futures as futures
import csv
import json
import os
import pathlib
import subprocess
import sys
import tempfile

MAX_ERROR_TAIL = 300  # chars of stderr kept per failed run

# Identity/constant columns first (no params swept here may reuse these
# names), then the swept keys, then all result columns.
IDENTITY_COLUMNS = ["strategy", "symbol", "market_type", "from", "to",
                    "quantity", "fee_rate", "cooldown_seconds", "close_mode"]
RESULT_COLUMNS = [
    "rows_total", "rows_sqlite", "rows_api", "missing_ranges",
    "candles_fed", "warmup_candles",
    "signal_count", "entry_signal_count", "trade_count",
    "net_pnl", "gross_profit", "gross_loss", "total_fees",
    "return_pct", "win_rate", "profit_factor", "max_drawdown_pct",
    "exit_code", "error",
]

# Pass-through flag → constant CSV column (the flag itself also reaches the
# binary verbatim in base_args).
BASE_PASSTHROUGH = {
    "--quantity": "quantity",
    "--fee-rate": "fee_rate",
    "--cooldown": "cooldown_seconds",
    "--market": "market_type",
    "--from": "from",
    "--to": "to",
}


# ---------------------------------------------------------------------------
# Path resolution
# ---------------------------------------------------------------------------

def find_binary():
    """Engine binary: $PULSE_BT_BIN, then build_headless/, then build/."""
    env = os.environ.get("PULSE_BT_BIN")
    if env:
        return env
    repo = pathlib.Path(__file__).resolve().parents[1]
    for cand in ("build_headless", "build"):
        binary = repo / cand / "apps" / "pulsetrader" / "pulsetrader"
        if binary.exists():
            return str(binary)
    sys.exit("engine binary not found — build first (cmake -B build_headless "
             "-DPULSE_ENABLE_SQLITE=ON ...) or set PULSE_BT_BIN")


# ---------------------------------------------------------------------------
# Value-spec parsing
# ---------------------------------------------------------------------------

def parse_value_spec(spec):
    """'5|9|21' → [5,9,21]; '34..100:11' → [34,45,...]; '2' → [2]."""
    if "|" in spec:
        values = []
        for part in spec.split("|"):
            part = part.strip()
            if not part:
                raise ValueError(f"empty discrete value in '{spec}'")
            values.append(float(part))
        return values
    if ".." in spec:
        body, _, step_text = spec.partition(":")
        start_text, _, end_text = body.partition("..")
        start = float(start_text)
        end = float(end_text)
        step = float(step_text) if step_text else 1.0
        if step <= 0.0 or end < start:
            raise ValueError(f"bad range '{spec}' (need from..to[:step], step>0)")
        values = []
        v = start
        while v <= end + 1e-9:
            values.append(v)
            v += step
        return values
    return [float(spec)]


def fmt_value(value):
    """Float formatting that keeps 5 → '5' and 0.0001 → '0.0001'."""
    return f"{value:g}"


# ---------------------------------------------------------------------------
# Identity / signatures
# ---------------------------------------------------------------------------

def signature_of(row, swept_keys):
    """Deterministic identity of one run: identity cols + every swept value."""
    parts = [str(row.get(c, "")) for c in IDENTITY_COLUMNS]
    for key in swept_keys:
        parts.append(str(row.get(key, "")))
    return "|".join(parts)


def header_for(swept_keys):
    return IDENTITY_COLUMNS + sorted(swept_keys) + RESULT_COLUMNS


# ---------------------------------------------------------------------------
# One backtest run
# ---------------------------------------------------------------------------

def run_one(binary, base_args, identity, combo, json_path):
    """Run one combo through the engine. Returns a full CSV row dict."""
    row = dict(identity)
    row.update({k: fmt_value(v) for k, v in combo.items()})

    cmd = [binary, "backtest"]
    cmd += base_args
    for key, value in combo.items():
        cmd += ["--param", f"{key}={value:g}"]
    cmd += ["--json", json_path]

    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
        row["exit_code"] = str(proc.returncode)
        if proc.returncode != 0:
            tail = proc.stderr.strip()[-MAX_ERROR_TAIL:]
            row["error"] = " ".join(tail.split())
            return row
        with open(json_path, encoding="utf-8") as fh:
            report = json.load(fh)
        data = report.get("data", {})
        stats = report.get("stats", {})
        row.update({
            "rows_total": data.get("rows_total", 0),
            "rows_sqlite": data.get("rows_sqlite", 0),
            "rows_api": data.get("rows_api", 0),
            "missing_ranges": data.get("missing_ranges", 0),
            "candles_fed": report.get("candles_fed", 0),
            "warmup_candles": report.get("warmup_candles", 0),
            "signal_count": stats.get("signal_count", 0),
            "entry_signal_count": stats.get("entry_signal_count", 0),
            "trade_count": stats.get("trade_count", 0),
            "net_pnl": stats.get("net_pnl", 0.0),
            "gross_profit": stats.get("gross_profit", 0.0),
            "gross_loss": stats.get("gross_loss", 0.0),
            "total_fees": stats.get("total_fees", 0.0),
            "return_pct": stats.get("return_pct", 0.0),
            "win_rate": stats.get("win_rate", 0.0),
            "profit_factor": stats.get("profit_factor", 0.0),
            "max_drawdown_pct": stats.get("max_drawdown_pct", 0.0),
        })
    except (subprocess.TimeoutExpired, json.JSONDecodeError, OSError) as exc:
        row["exit_code"] = "-1"
        row["error"] = f"{type(exc).__name__}: {exc}"[:MAX_ERROR_TAIL]
    return row


def run_bundle(bundle):
    """ProcessPool worker: unpacks a picklable bundle and runs one combo."""
    binary, base_args, identity, json_dir, run_id, combo = bundle
    json_path = os.path.join(json_dir, f"run_{run_id:04d}.json")
    return run_one(binary, base_args, identity, combo, json_path)


def write_csv(path, rows, swept_keys):
    columns = header_for(swept_keys)
    with open(path, "w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fieldnames=columns)
        writer.writeheader()
        for r in rows:
            writer.writerow(r)


def load_existing(path, swept_keys):
    """(rows, done_signatures) of an existing CSV; exits on schema mismatch."""
    if not pathlib.Path(path).exists():
        return [], set()
    with open(path, newline="", encoding="utf-8") as fh:
        reader = csv.DictReader(fh)
        if not reader.fieldnames:
            return [], set()
        if reader.fieldnames != header_for(swept_keys):
            sys.exit(f"existing {path} has a different schema than this sweep\n"
                     f"  file:    {reader.fieldnames}\n"
                     f"  current: {header_for(swept_keys)}\n"
                     "use --out for a new file or --force to overwrite")
        rows = list(reader)
    return rows, {signature_of(r, swept_keys) for r in rows}


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

# Flags the sweep parser understands itself (everything else passes through
# to the engine verbatim, with or without a value token).
VALUE_FLAGS = {"--strategy", "--symbol", "--param", "--max-combos", "--workers",
               "--out", "--json-dir"}
BOOLEAN_FLAGS = {"--force", "--help", "-h"}
# Engine flags that take NO value (passthrough detection needs to know).
PASSTHROUGH_BOOLEANS = {"--no-api", "--no-cache", "--no-contract-cache"}


def parse_args(argv):
    """Hand-rolled argv parsing (the C++ CLIs parse the same way): known
    sweep flags are consumed; anything else is passthrough, where a flag
    followed by a non-flag token carries that token as its value."""
    args = {"strategy": None, "symbol": None, "params": [], "max_combos": 500,
            "workers": 4, "out": None, "json_dir": None, "force": False,
            "pass_through": []}

    def value_for(i, flag):
        return argv[i + 1] if i + 1 < len(argv) else None

    i = 0
    while i < len(argv):
        tok = argv[i]
        if tok in BOOLEAN_FLAGS:
            if tok == "--help":
                print(__doc__)
                sys.exit(0)
            args["force"] = True
            i += 1
        elif tok in VALUE_FLAGS:
            val = value_for(i, tok)
            if val is None:
                sys.exit(f"{tok} requires a value")
            if tok == "--strategy":
                args["strategy"] = val
            elif tok == "--symbol":
                args["symbol"] = val
            elif tok == "--param":
                args["params"].append(val)
            elif tok == "--max-combos":
                args["max_combos"] = int(val)
            elif tok == "--workers":
                args["workers"] = int(val)
            elif tok == "--out":
                args["out"] = val
            elif tok == "--json-dir":
                args["json_dir"] = val
            i += 2
        elif tok.startswith("--"):
            # Passthrough: with a value when the next token is not a flag.
            args["pass_through"].append(tok)
            i += 1
            if i < len(argv) and not argv[i].startswith("--") \
                    and tok not in PASSTHROUGH_BOOLEANS:
                args["pass_through"].append(argv[i])
                i += 1
        else:
            sys.exit(f"unexpected positional argument '{tok}'")
    return args


def main():
    args = parse_args(sys.argv[1:])
    if not args["strategy"] or not args["symbol"]:
        sys.exit("--strategy and --symbol are required\n" + __doc__)

    pass_through = args["pass_through"]

    # --- Swept params + cartesian product ----------------------------------
    swept = []
    try:
        for spec in args["params"]:
            key, _, values_text = spec.partition("=")
            if not key or not values_text:
                sys.exit(f"--param expects KEY=SPEC, got '{spec}'")
            swept.append((key, parse_value_spec(values_text)))
    except ValueError as exc:
        sys.exit(f"--param error: {exc}")
    if not swept:
        sys.exit("no --param given — a sweep needs at least one swept parameter")

    combos = []
    def expand(idx, partial):
        if idx == len(swept):
            combos.append(dict(partial))
            return
        key, values = swept[idx]
        for v in values:
            expand(idx + 1, partial + [(key, v)])
    expand(0, [])

    if len(combos) > args["max_combos"]:
        sys.exit(f"{len(combos)} combos exceed --max-combos "
                 f"{args['max_combos']} (raise it if that is really what you "
                 "want)")
    swept_keys = [key for key, _ in swept]
    print(f"{args['strategy']} / {args['symbol']}: {len(combos)} combo(s) over "
          f"{', '.join(swept_keys)}")

    # --- Identity columns from the pass-through flags -----------------------
    identity = {"strategy": args["strategy"], "symbol": args["symbol"],
                "market_type": "futures",  # engine default unless --market
                "from": "", "to": "", "quantity": "", "fee_rate": "",
                "cooldown_seconds": "", "close_mode": ""}
    pending_key = None
    for tok in pass_through:
        if tok.startswith("--") and BASE_PASSTHROUGH.get(tok):
            pending_key = BASE_PASSTHROUGH[tok]
        elif pending_key:
            identity[pending_key] = tok
            pending_key = None

    binary = find_binary()
    base_args = ["--strategy", args["strategy"], "--symbol", args["symbol"]]
    base_args += pass_through

    # --- Output / resume ------------------------------------------------------
    out_path = args["out"] or os.path.join(
        "results", f"sweep_{args['strategy']}_{args['symbol']}.csv")
    pathlib.Path(out_path).parent.mkdir(parents=True, exist_ok=True)

    if args["force"] and pathlib.Path(out_path).exists():
        os.remove(out_path)
    existing, done_signatures = load_existing(out_path, swept_keys)

    def sig_for(combo):
        probe = dict(identity)
        probe.update({k: fmt_value(v) for k, v in combo.items()})
        return signature_of(probe, swept_keys)

    pending = []
    for combo in combos:
        sig = sig_for(combo)
        if sig in done_signatures:
            continue
        pending.append(combo)

    if not pending:
        print(f"nothing to do — all {len(combos)} combo(s) already in "
              f"{out_path} (use --force to re-run)")
        return

    json_dir = args["json_dir"] or tempfile.mkdtemp(prefix="pulse_sweep_")
    pathlib.Path(json_dir).mkdir(parents=True, exist_ok=True)

    # Warm-up on one serial run: its write-back fills kline_bars so the
    # parallel runs after it hit local rows only (rows_api ≈ 0).
    warm = pending[0]
    warm_text = " ".join(f"{k}={v:g}" for k, v in warm.items())
    print(f"[warm-up] {warm_text}")
    row = run_one(binary, base_args, identity, warm,
                  os.path.join(json_dir, "warmup.json"))
    existing.append(row)

    # Deterministic run ids (position in the full combo list) for the
    # run_*.json names; tuples are hashable, dicts are not.
    ordered = []
    for idx, combo in enumerate(combos, start=1):
        if sig_for(combo) not in done_signatures:
            ordered.append((idx, combo))
    rest = ordered[1:]  # first pending combo ran as the warm-up
    bundles = [(binary, base_args, identity, json_dir, run_id, combo)
               for run_id, combo in rest]

    done = 1
    with futures.ProcessPoolExecutor(max_workers=args["workers"]) as pool:
        for new_row in pool.map(run_bundle, bundles, chunksize=1):
            existing.append(new_row)
            done += 1
            write_csv(out_path, existing, swept_keys)  # crash-safe resume
            status = new_row.get("error") or f"pnl {new_row.get('net_pnl')}"
            print(f"[{done}/{len(combos)}] {status}")

    print(f"\n{done}/{len(combos)} runs → {out_path}")

    # --- Ranking --------------------------------------------------------------
    def num(value):
        try:
            return float(value)
        except (TypeError, ValueError):
            return 0.0

    traded = [r for r in existing
              if str(r.get("trade_count", "0")).isdigit()
              and int(r["trade_count"]) > 0 and not r.get("error")]
    if not traded:
        print("no rows with closed trades to rank")
        return
    for metric, label in (("net_pnl", "net PnL"), ("profit_factor", "profit factor")):
        print(f"\ntop-10 by {label}:")
        print("  {:<12} {:>12} {:>9} {:>8}  {}".format(
            "params", label, "return%", "win%", "window"))
        for r in sorted(traded, key=lambda x: num(x.get(metric)), reverse=True)[:10]:
            params = " ".join(f"{k}={r.get(k)}" for k in swept_keys if r.get(k))
            print("  {:<12} {:>12.3f} {:>8.1f}% {:>7.1f}%  {}..{}".format(
                params or "-", num(r.get(metric)),
                num(r.get("return_pct")) * 100, num(r.get("win_rate")) * 100,
                r.get("from", ""), r.get("to", "")))


if __name__ == "__main__":
    main()
