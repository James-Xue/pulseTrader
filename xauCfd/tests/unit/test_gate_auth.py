"""Unit tests for gate.auth — the signing scheme is the load-bearing detail.

These tests use a fixed timestamp so the HMAC bytes are reproducible. We
verify the canonical against the value we'd compute manually for a known
input (cross-checked against the official `gateapi-python` SDK).
"""
from __future__ import annotations

import hashlib
import hmac

import pytest

from xau.gate.auth import auth_headers, sign

FIXED_TS = "1754899200.123"
FIXED_SECRET = "test-secret-not-real"  # placeholder, NEVER use a real secret in tests
FIXED_KEY = "test-key-1234"


def test_canonical_uses_api_v4_path_prefix() -> None:
    """The signed path must include the /api/v4 prefix.

    If someone forgets this, the server returns INVALID_SIGNATURE even
    though the headers look correct.
    """
    sig_with_prefix = sign(
        FIXED_SECRET, "GET", "/api/v4/tradfi/users/mt5-account",
        "", "", timestamp=FIXED_TS,
    )
    sig_without_prefix = sign(
        FIXED_SECRET, "GET", "/tradfi/users/mt5-account",
        "", "", timestamp=FIXED_TS,
    )
    assert sig_with_prefix != sig_without_prefix
    # The first is what the server accepts (verified empirically 2026-08-11).
    assert len(sig_with_prefix) == 128  # SHA-512 hex = 128 chars


def test_canonical_matches_manual_construction() -> None:
    """Cross-check sign() against a hand-built HMAC.

    This is the test that catches "I changed the canonical string and
    nobody noticed". If it ever fails, the probe must be re-run.
    """
    method = "GET"
    path = "/api/v4/tradfi/users/mt5-account"
    query = ""
    body = ""
    ts = FIXED_TS

    body_hash = hashlib.sha512(b"").hexdigest()
    expected_canonical = "\n".join([method, path, query, body_hash, ts])
    expected_sig = hmac.new(
        FIXED_SECRET.encode(), expected_canonical.encode(), hashlib.sha512,
    ).hexdigest()

    actual = sign(FIXED_SECRET, method, path, query, body, timestamp=ts)
    assert actual == expected_sig
    # And the canonical itself is exactly what we expect.
    assert expected_canonical == (
        "GET\n/api/v4/tradfi/users/mt5-account\n\n"
        "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
        "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e"
        "\n1754899200.123"
    )


def test_method_is_uppercased_in_canonical() -> None:
    """The server expects METHOD uppercase in the canonical (verified via SDK)."""
    sig_lower = sign(FIXED_SECRET, "get", "/api/v4/tradfi/foo", "", "",
                     timestamp=FIXED_TS)
    sig_upper = sign(FIXED_SECRET, "GET", "/api/v4/tradfi/foo", "", "",
                     timestamp=FIXED_TS)
    assert sig_lower == sig_upper  # sign() uppercases internally


def test_query_string_changes_signature() -> None:
    """Adding query params must change the signature (canonical takes query)."""
    base = sign(FIXED_SECRET, "GET", "/api/v4/tradfi/orders",
                "", "", timestamp=FIXED_TS)
    with_q = sign(FIXED_SECRET, "GET", "/api/v4/tradfi/orders",
                  "status=open", "", timestamp=FIXED_TS)
    assert base != with_q


def test_body_changes_signature() -> None:
    """Body hash is part of the canonical — different bodies → different sigs."""
    a = sign(FIXED_SECRET, "POST", "/api/v4/tradfi/orders", "", '{"x":1}',
             timestamp=FIXED_TS)
    b = sign(FIXED_SECRET, "POST", "/api/v4/tradfi/orders", "", '{"x":2}',
             timestamp=FIXED_TS)
    assert a != b


def test_float_timestamp_required() -> None:
    """An integer-seconds timestamp must NOT match a float-seconds one.

    The server accepts only float timestamps in the canonical.
    """
    sig_float = sign(FIXED_SECRET, "GET", "/api/v4/tradfi/foo", "", "",
                     timestamp="1754899200.123")
    sig_int = sign(FIXED_SECRET, "GET", "/api/v4/tradfi/foo", "", "",
                   timestamp="1754899200")
    assert sig_float != sig_int


def test_timestamp_is_string_in_headers() -> None:
    """auth_headers returns the same string used in the canonical for Timestamp."""
    h = auth_headers(FIXED_KEY, FIXED_SECRET, "GET", "/api/v4/tradfi/foo",
                     timestamp=FIXED_TS)
    assert h["Timestamp"] == FIXED_TS
    # And the SIGN in the header matches what sign() would compute.
    assert h["SIGN"] == sign(FIXED_SECRET, "GET", "/api/v4/tradfi/foo",
                             "", "", timestamp=FIXED_TS)


def test_auth_headers_keys_are_bare_names() -> None:
    """Headers use bare KEY/SIGN/Timestamp — NOT GateIo-Api-* prefixed.

    Server rejects the prefixed names with MISSING_REQUIRED_HEADER.
    """
    h = auth_headers(FIXED_KEY, FIXED_SECRET, "GET", "/api/v4/tradfi/foo",
                     timestamp=FIXED_TS)
    assert set(h.keys()) == {"KEY", "SIGN", "Timestamp"}
    for k in ("GateIo-Api-Key", "GateIo-Api-Signature",
              "GateIo-Api-Timestamp", "GateIo-Api-Sign-Version"):
        assert k not in h


def test_timestamp_default_is_float_string() -> None:
    """When no timestamp is provided, we get a float-string like '1754899200.123'."""
    h = auth_headers(FIXED_KEY, FIXED_SECRET, "GET", "/api/v4/tradfi/foo")
    ts = h["Timestamp"]
    # Must contain a decimal point.
    assert "." in ts
    # And be parseable as a float close to current time.
    import time
    assert abs(float(ts) - time.time()) < 2.0


def test_signature_is_hex_not_base64() -> None:
    """Gate.io TradFi signatures are hex, not base64 (verified empirically)."""
    h = auth_headers(FIXED_KEY, FIXED_SECRET, "GET", "/api/v4/tradfi/foo",
                     timestamp=FIXED_TS)
    sig = h["SIGN"]
    # 128 hex chars for SHA-512, no base64 padding, no `+/=`.
    assert len(sig) == 128
    assert all(c in "0123456789abcdef" for c in sig)


@pytest.mark.parametrize("secret", [
    "short",
    "with-dashes-and_underscores",
    "abcdef0123456789" * 4,  # 64-char hex
    "has spaces in it",  # unusual but possible
])
def test_secret_used_as_raw_utf8_bytes(secret: str) -> None:
    """The secret is used as raw UTF-8 bytes (NOT base64-decoded)."""
    # Re-implement signing the same way to cross-check.
    body_hash = hashlib.sha512(b"").hexdigest()
    canonical = "\n".join(["GET", "/api/v4/tradfi/foo", "", body_hash, FIXED_TS])
    expected = hmac.new(secret.encode("utf-8"), canonical.encode("utf-8"),
                        hashlib.sha512).hexdigest()
    actual = sign(secret, "GET", "/api/v4/tradfi/foo", "", "",
                  timestamp=FIXED_TS)
    assert actual == expected