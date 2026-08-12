"""Run the xau-daemon with creds from Keychain.

Loads API key/secret via the same Keychain resolver as the live probe, then
launches the daemon with them set as env vars via os.environ — never echoed
to the shell command line. Run with:

    uv run python tools/run_daemon.py --once --live
    uv run python tools/run_daemon.py --live            # long-running mode
    uv run python tools/run_daemon.py --once --dry-run  # dry-run one cycle

The config file defaults to ./xau.toml; pass --config to override.
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from check_gate_cfd import _resolve_creds  # noqa: E402

REPO = Path(__file__).resolve().parents[1]


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--config", default="xau.toml")
    p.add_argument("--once", action="store_true")
    p.add_argument("--live", action="store_true")
    p.add_argument("--dry-run", action="store_true")
    p.add_argument("--min-bars", type=int, default=None)
    p.add_argument("--log-stdout", action="store_true",
                   help="Mirror daemon stderr to this terminal")
    args = p.parse_args()

    key, secret, _ = _resolve_creds()
    print(f"loaded creds (key prefix: {key[:6]}…)", flush=True)

    env = os.environ.copy()
    env["XAU_GATE_KEY"] = key
    env["XAU_GATE_SECRET"] = secret

    cmd = [sys.executable, "-m", "xau.apps.daemon", "--config", args.config]
    if args.once:
        cmd.append("--once")
    if args.live:
        cmd.append("--live")
    if args.dry_run:
        cmd.append("--dry-run")
    if args.min_bars is not None:
        cmd += ["--min-bars", str(args.min_bars)]
    print("running:", " ".join(cmd), flush=True)

    if args.log_stdout:
        return subprocess.call(cmd, cwd=str(REPO), env=env)
    return subprocess.call(cmd, cwd=str(REPO), env=env,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


if __name__ == "__main__":
    sys.exit(main())