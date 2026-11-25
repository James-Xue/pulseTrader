---
name: xau-llm-overlay
description: Write one 5-minute advisory XAUUSD analysis overlay (bias, regime, support/resistance, risk flags) for the local v10 paper runner. Analysis only; never submits an order or edits account files.
---

# XAUUSD 5-minute advisory overlay

This skill produces **analysis only**. It does not open/close/modify a paper
position, does not call `xau_signal.py`, and does not edit `sim_account.json`
or `sim_orders.db`.

The v10 Python runner keeps its own entry/stop/target logic. It reads the
overlay and may only **veto** a v10 open when:

- the overlay direction conflicts with v10 at sufficient confidence;
- `risk_flags` contains `no_trade`;
- the overlay says `regime=range` and the v10 entry is too close to the
  overlay's support (short) or resistance (long).

If the file is missing, stale, or invalid, v10 behaves exactly as before.

## Resolve paths first

Do not assume old drive letters. In the v10 work directory (`ce`), run:

```powershell
python xau_paths.py
```

It prints JSON with `work_dir`, `sim_data_dir`, `sim_app_dir`, `sim_exe`.
Alternatively read environment variables:

- `XAU_WORK_DIR`
- `PULSETRADER_SIM_DATA_DIR`
- `PULSETRADER_APP_DIR`
- `PULSETRADER_EXE`

Use `<work_dir>` and `<sim_data_dir>` below as the resolved values.

## One round

1. Read the current snapshot:
   - `<data_dir>\latest_snapshot.json`
   - If `generated_at` is older than 2 minutes or missing, run from
     `<work_dir>`:

     ```powershell
     python gold_snapshot.py --round-minutes 5
     ```

2. Read `<sim_data_dir>\external_state.json`. It is the source of truth for
   balance and open positions.
3. Analyze 1m/5m/15m/30m/1h/4h:
   - EMA20/EMA60 and price location;
   - MACD histogram/direction;
   - RSI14, ATR14, SuperTrend;
   - nearest support/resistance and range boundaries;
   - whether the market is trend or range.
4. Decide the **next 5–15 minutes** advisory bias:
   - `long`, `short`, or `neutral`;
   - `confidence` 0.0–1.0;
   - `regime` `trend`, `range`, or `transition`.

   Default to `neutral` with confidence ≤0.4 when unclear.
5. Write a temporary payload JSON (UTF-8) **outside the repository**, e.g.
   `$env:TEMP\xau_llm_overlay_<timestamp>.json`, then install it through the
   validator/writer so the runtime file is atomic and schema-checked:

   ```powershell
   python xau_llm_overlay.py --write-from "$env:TEMP\xau_llm_overlay_<timestamp>.json"
   ```

   Do not hand-edit the runtime overlay directly. Do not leave temp payloads
   in `ce` or in the git repository.
6. Optionally write a compact Markdown note
   `llm_overlay_<YYYY-MM-DD_HHMM>.md` in `<work_dir>` with the key levels and
   reasoning. The JSON remains the machine-readable source.
7. Report the installed bias, confidence, regime and whether the overlay is
   fresh. Do not send a trading signal.

## Payload template

```json
{
  "schema": "xau.llm_overlay/1",
  "symbol": "XAUUSD",
  "source": "claude-code",
  "generated_at": "2026-11-25T09:00:00+08:00",
  "valid_until": "2026-11-25T09:10:00+08:00",
  "bias": "neutral",
  "confidence": 0.30,
  "regime": "range",
  "support": [3615.0, 3612.0],
  "resistance": [3625.0, 3629.0],
  "entry_zone": [3616.0, 3619.0],
  "stop": 3612.0,
  "targets": [3626.5, 3629.0],
  "risk_flags": [],
  "reason": "区间上沿附近，方向未确认，等待回踩或突破"
}
```

Rules:

- `generated_at` and `valid_until` must include timezone; set `valid_until`
  to about **10 minutes** after `generated_at`.
- `bias`: `long | short | neutral`.
- `regime`: `trend | range | transition`.
- `confidence`: number 0..1. Direction conflict only vetoes at ≥0.60 by
  default.
- `support` / `resistance`: nearest 2–4 prices, not far-away levels.
- `entry_zone`: `[low, high]` or `null`.
- `stop` / `targets`: advisory only; v10 does not copy them.
- `risk_flags`: any of `no_trade`, `avoid_new`, `high_volatility`, `news`.
  Use `no_trade` only for a clear event/risk veto.
- Keep `reason` short and concrete.

## Start the 5-minute cadence

In a fresh Claude Code session, from the repository or a directory where this
project skill is discoverable:

```text
/loop 5m /xau-llm-overlay
```

Do **not** also run `/xau-5m` or `/xau-1m`: those old signal loops write
`gold.exec/1` orders and conflict with the v10 Python runner. This overlay
skill never writes orders.

The loop is session-only; keep the session open. If the skill name is not
discovered yet, restart Claude Code once.

## Hard rules

- Paper analysis only; never send a real order.
- Never call `xau_signal.py` from this skill.
- Never edit `sim_account.json`, `sim_orders.db`, or historical trade files.
- Do not let the LLM set lot size, leverage, or override risk limits.
- If no reliable view, write neutral/low confidence instead of forcing a
  direction.
