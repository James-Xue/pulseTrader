"""Gate.io TradFi CFD authentication signing.

The TradFi endpoints (`/tradfi/...`) follow the **unified v4 signing** scheme
used by the official `gateapi-python` SDK's `gen_sign`. We verified this
empirically on 2026-08-11 against /tradfi/users/mt5-account: integer-seconds
timestamps and SHA-256 variants both produce INVALID_SIGNATURE; only the
official SDK canonical (SHA-512, 5-field, `/api/v4` prefix in the signed path,
float timestamp) succeeds.

Canonical (newline-separated, 5 fields):
    {METHOD}\\n
    {PATH}\\n          # MUST include /api/v4 prefix
    {QUERY}\\n         # empty string if no query
    {SHA512_HEX(body)}\\n
    {TIMESTAMP}       # float seconds, e.g. "1754899200.123"

Headers (bare legacy names — server rejects `GateIo-Api-*` prefixed names
on TradFi endpoints with MISSING_REQUIRED_HEADER):
    KEY: <api_key>
    SIGN: <hex hmac>
    Timestamp: <same float string used in canonical>

Do NOT echo the secret in logs, errors, or test fixtures. Use a deterministic
test secret in unit tests; rely on `getpass` / Keychain in real entry points.
"""
from __future__ import annotations

import hashlib
import hmac
import time


def sign(
    secret: str,
    method: str,
    path: str,
    query: str,
    body: str,
    timestamp: str | None = None,
) -> str:
    """Compute the HMAC-SHA512 signature for a TradFi request.

    `path` MUST include the `/api/v4` prefix (it should equal
    `urlparse(full_url).path` where `full_url` is the request URL).
    `query` is the URL-encoded query string WITHOUT the leading `?`,
    or an empty string if the request has no query.
    `body` is the raw request body as a string (use `""` for GET / no body).
    `timestamp` defaults to the current float-second time; pass an explicit
    value in tests so the signature is reproducible.
    """
    ts = timestamp if timestamp is not None else repr(time.time())
    body_hash = hashlib.sha512(body.encode("utf-8")).hexdigest()
    canonical = "\n".join([method.upper(), path, query, body_hash, ts])
    return hmac.new(
        secret.encode("utf-8"), canonical.encode("utf-8"), hashlib.sha512,
    ).hexdigest()


def auth_headers(
    key: str,
    secret: str,
    method: str,
    path: str,
    query: str = "",
    body: str = "",
    timestamp: str | None = None,
) -> dict[str, str]:
    """Build the three required TradFi auth headers.

    Returned keys: `KEY`, `SIGN`, `Timestamp` — exactly as the server
    expects (bare names, NOT the `GateIo-Api-*` prefixed variants).
    """
    ts = timestamp if timestamp is not None else repr(time.time())
    sig = sign(secret, method, path, query, body, ts)
    return {
        "KEY": key,
        "SIGN": sig,
        "Timestamp": ts,
    }