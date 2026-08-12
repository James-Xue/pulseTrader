"""Run the integration smoke test with creds from Keychain.

Loads API key/secret via the same Keychain resolver as the live probe, then
launches pytest with them set as env vars via os.environ — never echoed to
the shell command line. Run with:

    uv run python tools/run_integration_smoke.py
"""
from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from check_gate_cfd import _resolve_creds  # noqa: E402

REPO = Path(__file__).resolve().parents[1]


def main() -> int:
    key, secret, _ = _resolve_creds()
    print(f"loaded creds (key prefix: {key[:6]}…)")
    env = os.environ.copy()
    env["XAU_GATE_KEY"] = key
    env["XAU_GATE_SECRET"] = secret
    cmd = [
        sys.executable, "-m", "pytest",
        "tests/integration/test_daemon_smoke.py",
        "-v", "--tb=short",
    ]
    print("running:", " ".join(cmd))
    return subprocess.call(cmd, cwd=str(REPO), env=env)


if __name__ == "__main__":
    sys.exit(main())