#!/usr/bin/env python3
"""make-vps-config.py — derive the VPS futures-only config from the repository's authoritative trading.toml.

Background (2026-09-04): after the mainnet engine moved to the VPS it runs a
futures-only config, because the mainnet API key has no tradfi/CFD permission yet
(403 FORBIDDEN). Manual derivation bit us twice: (1) forgetting to clear the local
Clash proxy proxyUrl=127.0.0.1:7897 left the engine unable to connect for market
data; (2) the derived artifact drifted from the repository config. This script turns
the derivation into a repeatable step: after changing the repository trading.toml in
future, just run it again to get the VPS version.

Usage:
    python3 deploy/make-vps-config.py [output path]
    Default output: deploy/cloud-out/trading.vps-futures.toml

Transformation rules:
    1. Clear proxyUrl (do not carry the local 7897 proxy over to the VPS)
    2. Drop XAUUSD / XAGUSD (CFD) from symbols
    3. active_market → futures
    4. Delete the whole TradFi CFD strategy block (from the comment header up to [risk])
    5. Validate with tomllib + print the instance list for manual checking
"""
import re
import sys
import tomllib
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SRC = REPO / "trading.toml"
OUT = Path(sys.argv[1]) if len(sys.argv) > 1 else REPO / "deploy/cloud-out/trading.vps-futures.toml"

s = SRC.read_text(encoding="utf-8")

# 1) Clear proxyUrl (the local Clash address)
s = re.sub(r'(proxyUrl\s*=\s*)"[^"]*"', r'\1""', s, count=1)

# 2) Drop the CFD instruments from symbols
s = re.sub(
    r"symbols = \[[^\]]*\]",
    'symbols = ["BTC_USDT", "SNDK_USDT", "UNITREE_USDT", "ETH_USDT"]',
    s,
    count=1,
)

# 3) active_market → futures (keep the comment in sync)
s = re.sub(
    r'active_market = "[^"]*"[^\n]*',
    'active_market = "futures"   # VPS derivation: the mainnet key has no tradfi/CFD permission yet (switch back to cfd and restore the full version once granted)',
    s,
    count=1,
)

# 4) Delete the TradFi CFD strategy block
start = s.index("# ── TradFi CFD strategies")
end = s.index("# ── Risk Management")
s = s[:start] + s[end:]

# 5) Validate + report
d = tomllib.loads(s)
n_cfd = sum(1 for i in d["strategy"]["instances"] if i.get("market_type") == "cfd")
assert n_cfd == 0, f"{n_cfd} CFD instances remain"
assert not any(x in d["symbols"] for x in ("XAUUSD", "XAGUSD")), "CFD symbols remain in symbols"
insts = sorted(f"{i['name']}_{i['symbol']}" for i in d["strategy"]["instances"])
OUT.parent.mkdir(parents=True, exist_ok=True)
OUT.write_text(s, encoding="utf-8")
print(f"Generated: {OUT}")
print(f"symbols:   {d['symbols']}")
print(f"active_market: {d['active_market']}")
print(f"proxyUrl:  {d['exchange'].get('proxyUrl', '')!r}")
print(f"instances: {len(insts)} (cfd={n_cfd})")
for i in insts:
    print(f"  - {i}")
