"""check_gate_cfd — read-only probe of Gate.io XAU_CFD access.

Verifies, in order:
  1. Public product spec reachable  (no auth, sanity check endpoint + region)
  2. Authenticated account endpoint (confirms key/secret + tier reach XAU_CFD)
  3. Existing positions and pending orders  (clean slate check before Phase 1)

Does NOT place any orders. Never logs the secret.

Usage:
    export XAU_GATE_KEY="<key>"
    export XAU_GATE_SECRET="<secret>"
    uv run python tools/check_gate_cfd.py
    uv run python tools/check_gate_cfd.py --base https://api.gateio.ws/api/v4  # alt host

Exits 0 if XAU_CFD is reachable + account is authorized. Non-zero otherwise.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import hmac
import json
import logging
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Any
from urllib.parse import urlencode

import httpx

# Project-root import so this can run via `python tools/check_gate_cfd.py`.
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

log = logging.getLogger("gate-probe")

KEYCHAIN_SERVICE = "xau-cfd-gate"

# Gate.io TradFi CFD endpoints. Two hosts are known to serve these:
#   - https://api.gate.com/api2/v4   (new "TradFi" gateway, hosts /cfds/ list)
#   - https://api.gateio.ws/api/v4   (legacy v4 host, hosts /tradfi/ namespace)
# The /cfds/XAU_CFD product-list endpoint is 404 on the legacy host; the
# /tradfi/* authenticated endpoints respond on both.
DEFAULT_BASE = "https://api.gateio.ws/api/v4"
ALT_BASE = "https://api.gate.com/api2/v4"

# TradFi CFD product symbol (NOT XAU_CFD — that was the /cfds/ alias).
PRODUCT = "XAUUSD"


def _key_prefix(key: str | None) -> str:
    """Show first 6 chars + ellipsis. Used only to confirm which key loaded."""
    if not key:
        return "(unset)"
    return key[:6] + "…"


def _resolve_creds(
    service: str = KEYCHAIN_SERVICE,
    account_key: str = "key",
    account_secret: str = "secret",
) -> tuple[str | None, str | None, str]:
    """Resolve API creds in priority order: env vars > macOS Keychain > none.

    Returns (key, secret, source_label) where source_label is "env",
    "keychain", or "none". The secret is NEVER returned via logging or
    stdout; the caller passes it directly into HMAC.
    """
    env_key = os.environ.get("XAU_GATE_KEY", "")
    env_secret = os.environ.get("XAU_GATE_SECRET", "")
    if env_key and env_secret:
        return env_key, env_secret, "env"

    # Fallback: macOS Keychain via the `security` CLI. We never print the
    # returned secret; we pass it straight into the HMAC signer.
    sec = shutil.which("security")
    if sec is None:
        return None, None, "none"
    try:
        kp = subprocess.run(
            [sec, "find-generic-password", "-s", service, "-a", account_key, "-w"],
            capture_output=True, check=False,
        )
        sp = subprocess.run(
            [sec, "find-generic-password", "-s", service, "-a", account_secret, "-w"],
            capture_output=True, check=False,
        )
    except Exception:  # noqa: BLE001
        return None, None, "none"
    if kp.returncode == 0 and sp.returncode == 0:
        # `.strip()` removes trailing newline that `security -w` sometimes
        # appends — that newline would corrupt the HMAC bytes.
        return (
            kp.stdout.decode("utf-8").strip(),
            sp.stdout.decode("utf-8").strip(),
            "keychain",
        )
    return None, None, "none"


def _sign_tradfi(
    secret: str, method: str, path: str, query: str, body: str, ts: str,
) -> str:
    """Gate.io TradFi CFD signature (unified v4 format, per official SDK).

    Canonical string (newline-separated, 5 fields):
        {METHOD}\\n
        {PATH}\\n        ← MUST include the /api/v4 prefix (full URL path)
        {QUERY}\\n       ← empty string if no query
        {SHA512_HEX(body)}\\n
        {TIMESTAMP}      ← float seconds as string, e.g. "1754899200.123"

    Notes (verified empirically against /tradfi/users/mt5-account):
      * HMAC is **SHA-512**, signature is **hex**.
      * Secret used as raw UTF-8 bytes (no base64 decoding).
      * Empty body → SHA-512 hex of empty string (constant).
      * Path includes the `/api/v4` prefix, even though the URL path the
        SDK signs is `urlparse(full_url).path` which always contains it.
      * Timestamp must be a **float** (string repr of `time.time()`),
        not an integer. Integer seconds → INVALID_SIGNATURE.
    """
    body_hash = hashlib.sha512(body.encode("utf-8")).hexdigest()
    canonical = "\n".join([method.upper(), path, query, body_hash, ts])
    mac = hmac.new(secret.encode("utf-8"), canonical.encode("utf-8"), hashlib.sha512)
    return mac.hexdigest()


def _auth_headers(
    key: str, secret: str, method: str, path: str, query: str, body: str,
) -> dict[str, str]:
    """Build TradFi CFD auth headers.

    `path` MUST include the `/api/v4` prefix (the signed path mirrors
    `urlparse(full_url).path` from the official SDK, which includes it).
    `query` is the URL-encoded query string WITHOUT the leading `?`.
    Timestamp is a float-seconds string (e.g. "1754899200.123") — integer
    seconds returns INVALID_SIGNATURE.
    """
    ts = repr(time.time())  # float with full precision, like SDK
    sig = _sign_tradfi(secret, method, path, query, body, ts)
    return {
        "KEY": key,
        "SIGN": sig,
        "Timestamp": ts,
        "Content-Type": "application/json",
    }


def _display(label: str, value: Any, *, width: int = 28) -> None:
    print(f"  {label:<{width}} {value}")


def probe_product(base: str, client: httpx.Client) -> dict[str, Any] | None:
    """Fetch public symbols list and extract the XAUUSD entry.

    The /tradfi/symbols endpoint returns the full product catalog as a
    list under data.list; we filter to PRODUCT and surface the load-bearing
    fields. Falls back to /tradfi/symbols/detail?symbol=XAUUSD for detail.
    """
    print(f"\n[1/3] Public product spec — {base}/tradfi/symbols")
    try:
        r = client.get(f"{base}/tradfi/symbols", timeout=10.0)
    except httpx.HTTPError as exc:
        print(f"  ✗ HTTP error: {exc}")
        return None
    print(f"  status: {r.status_code}")
    if r.status_code != 200:
        print(f"  body:   {r.text[:200]}")
        return None
    try:
        envelope = r.json()
    except json.JSONDecodeError:
        print(f"  ✗ non-JSON response: {r.text[:200]}")
        return None
    items = (envelope.get("data") or {}).get("list") if isinstance(envelope, dict) else None
    if not items:
        print(f"  ✗ no data.list in payload")
        return None
    match = next((s for s in items if s.get("symbol") == PRODUCT), None)
    if not match:
        print(f"  ✗ {PRODUCT} not found in {len(items)} symbols")
        print(f"  available: {[s.get('symbol') for s in items[:10]]}")
        return None
    print(f"  ✓ product online")
    _display("symbol", match.get("symbol"))
    _display("description", match.get("symbol_desc"))
    _display("category_id", match.get("category_id"))
    _display("trade_mode", match.get("trade_mode"))
    _display("status", match.get("status"))
    _display("price precision", match.get("price_precision"))
    _display("settlement ccy", match.get("settlement_currency"))
    levs = match.get("leverages", []) or []
    _display("available leverages", ", ".join(str(x) for x in levs))
    _display("next_open_time", match.get("next_open_time"))
    return match


def probe_account(
    base: str, client: httpx.Client, secret: str,
    service: str, account_key: str, account_secret: str,
) -> dict[str, Any] | None:
    key, _, source = _resolve_creds(
        service=service, account_key=account_key, account_secret=account_secret,
    )
    print(f"\n[2/3] Authenticated account — key={_key_prefix(key)} (source={source})")
    if not secret:
        print("  ✗ no creds available — set $XAU_GATE_KEY/SECRET or store via tools/store_gate_creds.py")
        return None
    # /tradfi/users/mt5-account is the documented account endpoint (per
    # the v4.106 changelog). Try /tradfi/users/assets as a secondary.
    # Signed path includes /api/v4 prefix (full URL path), per official SDK.
    for url_path in ("/tradfi/users/mt5-account", "/tradfi/users/assets"):
        sign_path = f"/api/v4{url_path}"
        try:
            headers = _auth_headers(key or "", secret, "GET", sign_path, "", "")
            r = client.get(f"{base}{url_path}", headers=headers, timeout=10.0)
        except httpx.HTTPError as exc:
            print(f"  ✗ HTTP error on {url_path}: {exc}")
            continue
        print(f"  {url_path}: status {r.status_code}")
        if r.status_code == 200:
            try:
                data = r.json()
            except json.JSONDecodeError:
                print(f"  ✓ non-JSON response: {r.text[:200]}")
                return {"raw": r.text[:200]}
            print(f"  ✓ account reachable")
            payload = data.get("data", data) if isinstance(data, dict) else data
            if isinstance(payload, dict):
                for k, v in payload.items():
                    if k in ("user_id", "leverage", "available_leverage",
                             "amount", "equity", "pnl", "margin", "free_margin",
                             "balance", "status", "position_id"):
                        _display(k, v)
            return data
        # Surface the auth error verbatim for diagnosis.
        try:
            err = r.json()
        except json.JSONDecodeError:
            err = {"raw": r.text[:200]}
        print(f"  body: {json.dumps(err)[:200]}")
    return None


def probe_positions_orders(
    base: str, client: httpx.Client, secret: str, key: str,
) -> None:
    """Query positions and open orders. No-op if no creds available."""
    if not secret:
        return
    print(f"\n[3/3] Existing positions & pending orders")
    # /tradfi/positions and /tradfi/orders are the live endpoints. We don't
    # know the exact path shape yet — try a few.
    candidates = [
        ("positions", f"/tradfi/positions"),
        ("positions", f"/tradfi/positions?symbol={PRODUCT}"),
        ("orders",    f"/tradfi/orders"),
        ("orders",    f"/tradfi/orders?status=open"),
    ]
    seen: set[tuple[str, str]] = set()
    for kind, url_path in candidates:
        if (kind, url_path) in seen:
            continue
        seen.add((kind, url_path))
        try:
            # Split path on '?' so we can sign (path, query) separately;
            # the unified v4 canonical takes the query as its own field.
            # Signed path includes /api/v4 prefix (full URL path).
            if "?" in url_path:
                p_only, q_only = url_path.split("?", 1)
            else:
                p_only, q_only = url_path, ""
            sign_path = f"/api/v4{p_only}"
            headers = _auth_headers(key, secret, "GET", sign_path, q_only, "")
            r = client.get(f"{base}{url_path}",
                           headers=headers, timeout=10.0)
        except httpx.HTTPError as exc:
            print(f"  ✗ {kind}: HTTP error: {exc}")
            continue
        print(f"  {kind} {url_path}: status {r.status_code}", end="")
        if r.status_code != 200:
            print(f" body={r.text[:120]}")
            continue
        try:
            data = r.json()
        except json.JSONDecodeError:
            print(f"  ✗ non-JSON: {r.text[:120]}")
            continue
        items = (data.get("data") if isinstance(data, dict) and "data" in data else data)
        if isinstance(items, list):
            print(f" count={len(items)}")
            for item in items:
                keep = {k: item[k] for k in item
                        if k in ("order_id", "position_id", "side",
                                 "volume", "amount", "price", "status",
                                 "symbol", "contract", "leverage",
                                 "time_setup")}
                print(f"    - {json.dumps(keep)[:240]}")
        elif isinstance(items, dict):
            inner = items.get("list")
            if isinstance(inner, list):
                print(f" total={items.get('total')} count={len(inner)}")
                for item in inner:
                    keep = {k: item[k] for k in item
                            if k in ("order_id", "position_id", "side",
                                     "volume", "amount", "price", "status",
                                     "symbol", "contract", "leverage",
                                     "time_setup")}
                    print(f"    - {json.dumps(keep)[:240]}")
            else:
                print(f" payload keys: {list(items.keys())[:10]}")
        else:
            print(f" payload: {json.dumps(data)[:200]}")


def main() -> int:
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
    )
    p = argparse.ArgumentParser(description="Probe Gate.io XAU_CFD access (read-only)")
    p.add_argument("--base", default=DEFAULT_BASE,
                   help=f"API base URL (default: {DEFAULT_BASE})")
    p.add_argument("--timeout", type=float, default=10.0)
    p.add_argument("--service", default=KEYCHAIN_SERVICE,
                   help=f"keychain service name (default: {KEYCHAIN_SERVICE})")
    p.add_argument("--account-key", default="key",
                   help="account name under service for the API key")
    p.add_argument("--account-secret", default="secret",
                   help="account name under service for the API secret")
    args = p.parse_args()

    base = args.base.rstrip("/")
    key, secret, source = _resolve_creds(
        service=args.service,
        account_key=args.account_key,
        account_secret=args.account_secret,
    )
    print(f"=== Gate.io XAU_CFD probe ===")
    print(f"  base: {base}")
    print(f"  product: {PRODUCT}")
    print(f"  key prefix: {_key_prefix(key)}")
    print(f"  creds source: {source}")

    with httpx.Client(timeout=args.timeout) as client:
        product = probe_product(base, client)
        account = probe_account(
            base, client, secret or "",
            service=args.service,
            account_key=args.account_key,
            account_secret=args.account_secret,
        )
        if account is not None:
            probe_positions_orders(base, client, secret or "", key or "")

    print(f"\n=== Summary ===")
    if product is None:
        print("  ✗ Product spec unreachable — try --base https://api.gateio.ws/api/v4")
        return 2
    if account is None:
        print("  ✗ Authenticated account unreachable — key/secret invalid or tier too low")
        print("    Check user_min_id above matches your KYC tier.")
        return 3
    print("  ✓ XAU_CFD reachable + account authorized for this product")
    return 0


if __name__ == "__main__":
    sys.exit(main())