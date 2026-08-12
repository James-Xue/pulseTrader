"""Smoke test for the daemon — one dry-run cycle against the live broker.

This test uses the real Gate.io REST API (creds from Keychain / env) but
NEVER places orders. It verifies that:
  * ticker polling works
  * synthetic bars accumulate
  * the planner produces at least one planned item OR logs "warmup"
    (depends on whether the market is resting on a clear level right now)

Skipped if no credentials are available — runs only when XAU_GATE_KEY env
is set (CI-safe). The poll_interval_seconds and min_bars are tight (1s,
5 bars) to keep the test under ~60s wall time.

NEVER log the API secret.
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

import pytest


REPO_ROOT = Path(__file__).resolve().parents[2]


def _has_creds() -> bool:
    return os.environ.get("XAU_GATE_KEY") is not None and \
        os.environ.get("XAU_GATE_SECRET") is not None


pytestmark = pytest.mark.skipif(
    not _has_creds(),
    reason="XAU_GATE_KEY/SECRET not set — skipping live smoke test",
)


def _write_test_config(tmp_path: Path) -> Path:
    cfg = tmp_path / "xau.toml"
    cfg.write_text(
        "[broker]\n"
        'symbol = "XAUUSD"\n'
        "leverage = 500\n"
        'base_url = "https://api.gateio.ws/api/v4"\n'
        "req_timeout_s = 5.0\n"
        "[credentials]\n"
        'api_key = "from_env:XAU_GATE_KEY"\n'
        'api_secret = "from_env:XAU_GATE_SECRET"\n'
        "[connection]\n"
        "poll_interval_seconds = 1.0\n"
        "[profile]\n"
        "window_bars = 200\n"
        "bar_seconds = 10\n"
        'bin_usd = 0.10\n'
        "[strategy]\n"
        "top_n_levels = 6\n"
        "ema_short = 4\n"           # short for smoke
        "ema_long = 6\n"             # short for smoke
        "trend_threshold_pct = 0.05\n"
        "confidence_full_pct = 0.30\n"
        "offset_min_usd = 0.03\n"
        "offset_max_usd = 0.05\n"
        "min_distance_usd = 0.30\n"
        "max_distance_usd = 0.80\n"
        "min_bars_for_plan = 6\n"    # 6 bars is enough for a smoke plan
        "[daemon]\n"
        "dry_run = true\n"
        f'log_json_path = "{tmp_path}/daemon.jsonl"\n'
        'log_level = "INFO"\n'
        "[risk]\n"
        "risk_pct = 0.066\n"
        'sl_usd = 0.60\n'
        "max_lot = 0.10\n"
        "min_lot = 0.01\n",
        encoding="utf-8",
    )
    return cfg


def test_daemon_dry_run_one_cycle(tmp_path: Path) -> None:
    cfg_path = _write_test_config(tmp_path)
    log_path = tmp_path / "daemon.jsonl"

    env = os.environ.copy()
    # Run the daemon with --once and --dry-run.
    result = subprocess.run(
        [
            sys.executable, "-m", "xau.apps.daemon",
            "--config", str(cfg_path),
            "--once",
            "--dry-run",
            "--min-bars", "6",
        ],
        cwd=str(REPO_ROOT),
        env=env,
        capture_output=True,
        text=True,
        timeout=180,
    )
    # If daemon failed, surface a clean error.
    assert result.returncode == 0, (
        f"daemon exited {result.returncode}\n"
        f"STDOUT:\n{result.stdout[-2000:]}\n"
        f"STDERR:\n{result.stderr[-2000:]}"
    )

    # The JSON log file should exist and have at least one structured line.
    assert log_path.exists(), f"expected log file at {log_path}"
    lines = log_path.read_text().strip().splitlines()
    assert len(lines) >= 1, "log file empty"

    # The first few events should include warmup + at least one market_state.
    events = [json.loads(line) for line in lines if line.startswith("{")]
    kinds = {ev.get("event") for ev in events if "event" in ev}
    assert "warmup_start" in kinds or "warmup_done" in kinds, \
        f"expected warmup events, got {kinds}"

    # Look for "market_state" — confirms ticker polling succeeded.
    market_states = [ev for ev in events if ev.get("event") == "market_state"]
    assert len(market_states) >= 1, \
        f"expected at least one market_state event, got {events[:5]}"

    # We may or may not get a plan_emitted depending on whether the market
    # is resting on a clear level at smoke-test time. Verify the plan
    # pipeline ran (plan_emitted event) and check the dry_run_plan events
    # if any.
    plan_emitted = [ev for ev in events if ev.get("event") == "plan_emitted"]
    assert len(plan_emitted) >= 1, "expected at least one plan_emitted event"
    plans = [ev for ev in events if ev.get("event") == "dry_run_plan"]
    if plans:
        for p in plans:
            assert p["side"] in ("BUY", "SELL")
            assert float(p["price"]) > 0
            assert float(p["lot"]) >= 0.01


def test_daemon_handles_missing_secret(tmp_path: Path) -> None:
    """If credentials are missing, daemon should fail loudly at config load."""
    cfg = tmp_path / "xau.toml"
    cfg.write_text(
        "[broker]\n"
        'symbol = "XAUUSD"\n'
        "leverage = 500\n"
        "[credentials]\n"
        'api_key = "from_env:XAU_DEFINITELY_NOT_SET_XYZ"\n'
        'api_secret = "from_env:XAU_DEFINITELY_NOT_SET_XYZ"\n'
        "[risk]\nrisk_pct=0.066\nsl_usd=0.60\nmax_lot=0.10\nmin_lot=0.01\n"
        "[strategy]\nema_short=5\nema_long=10\n",
        encoding="utf-8",
    )
    env = {k: v for k, v in os.environ.items()
           if k not in ("XAU_GATE_KEY", "XAU_GATE_SECRET",
                        "XAU_DEFINITELY_NOT_SET_XYZ")}
    result = subprocess.run(
        [sys.executable, "-m", "xau.apps.daemon", "--config", str(cfg),
         "--once", "--dry-run"],
        cwd=str(REPO_ROOT), env=env,
        capture_output=True, text=True, timeout=10,
    )
    assert result.returncode != 0
    # Config loader raises KeyError mentioning the missing var.
    combined = result.stdout + result.stderr
    assert "XAU_DEFINITELY_NOT_SET_XYZ" in combined