"""store_gate_creds — put Gate.io API key + secret into macOS Keychain.

Why Keychain (over `.env`):
  * Encrypted at rest by the OS, gated by your macOS user login
  * Not synced to git, not in shell history, not in chat scrollback
  * Can be revoked in one click from Keychain Access.app
  * Survives reboots and shell sessions

This script uses the built-in `security` CLI (no Python deps). It is
interactive — your secret is read via `getpass`, never echoed to the
terminal, never logged, never sent over the network. We `add` with
`-U` (update-if-exists) so re-running replaces the existing entry.

Usage:
    uv run python tools/store_gate_creds.py set      # prompt + save
    uv run python tools/store_gate_creds.py check    # verify presence (no values)
    uv run python tools/store_gate_creds.py delete   # remove both entries
    uv run python tools/store_gate_creds.py list     # show how to enumerate items

If you already have items under a different service/account name (e.g.
you added "CFD"/"Key"/"Secret" by hand in Keychain Access.app), pass
    --service CFD --account-key Key --account-secret Secret
and the probe will read them under those names. Re-running `set` with
the same flags will overwrite your existing entries in-place.
"""
from __future__ import annotations

import argparse
import getpass
import shutil
import subprocess
import sys
from dataclasses import dataclass

DEFAULT_SERVICE = "xau-cfd-gate"
DEFAULT_ACCOUNT_KEY = "key"
DEFAULT_ACCOUNT_SECRET = "secret"


@dataclass(frozen=True, slots=True)
class KeychainTarget:
    service: str
    account_key: str
    account_secret: str


def _security() -> str:
    path = shutil.which("security")
    if path is None:
        sys.exit(
            "✗ `security` CLI not found. This script is macOS-only.\n"
            "  On Linux use `secret-tool` (libsecret); on Windows use `cmdkey`."
        )
    return path


def _run(cmd: list[str]) -> subprocess.CompletedProcess:
    proc = subprocess.run(cmd, capture_output=True, check=False)
    if proc.returncode != 0:
        err = proc.stderr.decode("utf-8", errors="replace").strip()
        raise SystemExit(f"✗ security failed (exit {proc.returncode}): {err}")
    return proc


def _run_soft(cmd: list[str]) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, check=False)


def cmd_set(t: KeychainTarget) -> None:
    sec = _security()
    print(f"→ saving to Keychain under service: {t.service!r}")
    print(f"  account key    = {t.account_key!r}")
    print(f"  account secret = {t.account_secret!r}")
    print("  (entries appear in Keychain Access.app under that service name)\n")

    key = input("  API KEY (paste, will be stored): ").strip()
    if not key:
        sys.exit("✗ empty key, aborting")

    secret1 = getpass.getpass("  API SECRET (hidden): ")
    if not secret1:
        sys.exit("✗ empty secret, aborting")
    secret2 = getpass.getpass("  confirm API SECRET: ")
    if secret1 != secret2:
        sys.exit("✗ secret mismatch, aborting (nothing saved)")

    # `-U` updates the entry if it already exists.
    _run([sec, "add-generic-password", "-U",
          "-s", t.service, "-a", t.account_key, "-w", key])
    _run([sec, "add-generic-password", "-U",
          "-s", t.service, "-a", t.account_secret, "-w", secret1])
    print(f"\n✓ saved. verify with: python tools/store_gate_creds.py check")


def cmd_check(t: KeychainTarget) -> None:
    sec = _security()
    found_key = False
    found_secret = False
    for account, label, is_key in (
        (t.account_key, "API KEY", True),
        (t.account_secret, "API SECRET", False),
    ):
        proc = _run_soft([sec, "find-generic-password", "-s", t.service, "-a", account])
        if proc.returncode == 0:
            print(f"  ✓ {label}: present (service={t.service}, account={account})")
            if is_key:
                found_key = True
            else:
                found_secret = True
        else:
            print(f"  ✗ {label}: missing (service={t.service}, account={account})")
    if not (found_key and found_secret):
        sys.exit(2)
    print(f"\n✓ both entries present. probe will read them automatically.")


def cmd_delete(t: KeychainTarget) -> None:
    sec = _security()
    for account in (t.account_key, t.account_secret):
        proc = _run_soft([sec, "delete-generic-password", "-s", t.service, "-a", account])
        if proc.returncode == 0:
            print(f"  ✓ deleted service={t.service} account={account}")
        else:
            err = proc.stderr.decode("utf-8", errors="replace").strip()
            print(f"  · service={t.service} account={account} not present ({err})")


def cmd_list(t: KeychainTarget) -> None:
    """Show the one-liner to enumerate items in this service.

    The macOS `security` CLI has no direct "list by service" command; the
    standard workaround is to grep the dump-keychain output or to use
    Keychain Access.app. We don't print any values."""
    print(f"  Items under service={t.service!r} can be inspected via:")
    print(f"    security dump-keychain 2>/dev/null | grep -A2 'svce=<{t.service}>'")
    print(f"  Or open Keychain Access.app and search for {t.service!r}.")


def main() -> int:
    p = argparse.ArgumentParser(
        description="Manage Gate.io API creds in macOS Keychain",
    )
    p.add_argument("--service", default=DEFAULT_SERVICE,
                   help=f"keychain service name (default: {DEFAULT_SERVICE})")
    p.add_argument("--account-key", default=DEFAULT_ACCOUNT_KEY,
                   help=f"account name for the API key (default: {DEFAULT_ACCOUNT_KEY})")
    p.add_argument("--account-secret", default=DEFAULT_ACCOUNT_SECRET,
                   help=f"account name for the API secret (default: {DEFAULT_ACCOUNT_SECRET})")
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("set", help="prompt + save key + secret")
    sub.add_parser("check", help="verify presence (no values shown)")
    sub.add_parser("delete", help="remove both entries")
    sub.add_parser("list", help="show how to enumerate items in this service")
    args = p.parse_args()

    target = KeychainTarget(
        service=args.service,
        account_key=args.account_key,
        account_secret=args.account_secret,
    )

    if args.cmd == "set":
        cmd_set(target)
    elif args.cmd == "check":
        cmd_check(target)
    elif args.cmd == "delete":
        cmd_delete(target)
    elif args.cmd == "list":
        cmd_list(target)
    return 0


if __name__ == "__main__":
    sys.exit(main())