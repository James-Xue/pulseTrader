"""TOML config loader with `from_env:` placeholder substitution.

The config file is plain TOML. Values of the form `"from_env:VAR_NAME"` are
replaced with the value of environment variable `VAR_NAME` at load time. This
is how secrets (API keys) flow into config without being committed.

Example `xau.toml`:
    [broker]
    api_key = "from_env:XAU_GATE_KEY"
    api_secret = "from_env:XAU_GATE_SECRET"

Load:
    cfg = load_config(Path("xau.toml"))
    cfg["broker"]["api_key"]    # resolved from env

Optional `from_env:VAR:-default` form falls back to `default` if the env var
is unset (useful for optional overrides).
"""
from __future__ import annotations

import os
import re
import sys
from pathlib import Path
from typing import Any

if sys.version_info >= (3, 11):
    import tomllib as _toml
else:  # pragma: no cover — pyproject pins 3.11+
    import tomli as _toml  # type: ignore[no-redef]


_ENV_RE = re.compile(r"^from_env:([A-Z_][A-Z0-9_]*)(?::-(.*))?$")


def _resolve(value: Any) -> Any:
    """Recursively replace `from_env:VAR` strings with env values."""
    if isinstance(value, str):
        m = _ENV_RE.match(value)
        if m:
            var, default = m.group(1), m.group(2)
            resolved = os.environ.get(var, default if default is not None else "")
            if resolved == "" and default is None:
                raise KeyError(f"required env var {var} not set")
            return resolved
        return value
    if isinstance(value, list):
        return [_resolve(v) for v in value]
    if isinstance(value, dict):
        return {k: _resolve(v) for k, v in value.items()}
    return value


def load_config(path: Path | str) -> dict[str, Any]:
    """Load and resolve a TOML config file. Raises on missing file or bad TOML."""
    p = Path(path)
    if not p.exists():
        raise FileNotFoundError(f"config not found: {p}")
    with p.open("rb") as f:
        raw = _toml.load(f)
    return _resolve(raw)


def section(cfg: dict[str, Any], name: str) -> dict[str, Any]:
    """Return `cfg[name]` or an empty dict. Never raises for a missing section."""
    val = cfg.get(name)
    if val is None:
        return {}
    if not isinstance(val, dict):
        raise TypeError(f"config section [{name}] must be a table, got {type(val).__name__}")
    return val