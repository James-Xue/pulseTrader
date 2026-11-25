---
name: gate-sim
description: Launch or focus the local Qt "模拟gate系统" paper-trading app, auto-login from .env, auto-start the price engine, and report engine/heartbeat status. Paper account only; never sends a real order.
---

# Launch the Gate simulation app

This project skill is the portable copy used when the pulseTrader repository is
checked out on another machine. It reads paths from `xau_config.json` or
environment variables; it does not assume the old machine's drive letters.

Path precedence:

1. `PULSETRADER_EXE`, `PULSETRADER_APP_DIR`, `PULSETRADER_SIM_DATA_DIR`
2. `xau_config.json` found through `XAU_WORK_DIR`
3. `xau_config.json` next to the skill / repository

The launcher script is:

```text
.claude/skills/gate-sim/scripts/start_gate_sim.ps1
```

## Procedure

1. Ensure `xau_config.json` exists or the `PULSETRADER_*` environment
   variables are set. Example config:

   ```json
   {
     "work_dir": "D:/quant/quantification_platform/ce",
     "data_dir": "D:/quant/quantification_platform/xau_data",
     "sim_data_dir": "D:/quant/quantification_platform/xau_data/sim",
     "sim_app_dir": "D:/quant/pulseTrader/build/apps/pulsetrader_ui",
     "sim_exe": "D:/quant/pulseTrader/build/apps/pulsetrader_ui/Debug/pulsetrader_ui.exe"
   }
   ```

2. Run:

   ```powershell
   powershell -NoProfile -ExecutionPolicy Bypass -File `
     "<REPO_ROOT>\.claude\skills\gate-sim\scripts\start_gate_sim.ps1"
   ```

3. Report:
   - `gate-sim: started PID=...` or `already running PID=...`
   - `engine: running (...)` or `engine: not confirmed (...)`
   - heartbeat fresh/stale/missing

4. If the engine is not confirmed within 25 seconds, check the Qt window:
   - `.env` missing or in the wrong working directory;
   - invalid/expired paper API key;
   - network/proxy failure;
   - login dialog waiting for input.

   Do not print or inspect credential values.

5. If an instance is already running, focus it and do not start a duplicate.
   Do not stop it unless the user explicitly asks.

## What the launcher does

It starts `pulsetrader_ui.exe` with:

```text
--auto-login --auto-start-engine
```

- `--auto-login` reads `GATE_MAINNET_API_KEY` / `GATE_MAINNET_API_SECRET`
  (legacy `GATE_API_KEY` / `GATE_API_SECRET` also works) from `.env` in the
  launcher working directory.
- `--auto-start-engine` starts the price polling engine after the main window
  appears.
- The old built-in strategies are disabled. Trading is driven by external
  `gold.exec/1` signals only.

## Important

- This skill only starts Qt. It does **not** start v10, the queue consumer, or
  any signal loop.
- Start the v10 runner separately:
  `python xau_model_fast.py --loop ... --strategy-id v10 --signal-queue`.
- Do not start the old `xau-5m` / `xau-1m` signal loops while v10 owns entries;
  they write `gold.exec/1` orders and can conflict.
- Paper account only. Never send a real Gate order.
- Never cat/echo `.env` or print API secrets.
- Never edit `sim_account.json` or `sim_orders.db` directly.
