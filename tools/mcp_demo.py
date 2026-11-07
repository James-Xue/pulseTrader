#!/usr/bin/env python3
"""Drive the engine's MCP bridge from a script and print a readable transcript.

This is the demo used in docs/demo-mcp.md. It starts the sample-data engine,
starts `pulsetrader mcp` as a stdio bridge, then speaks MCP to it exactly as an
LLM client would: discover, list tools, call a few of them.

No exchange keys, no funded account, no trading engine build required beyond
the `pulsetrader` binary.

Usage:
    python3 tools/mcp_demo.py                 # human-readable transcript
    python3 tools/mcp_demo.py --json          # raw request/response lines
    python3 tools/mcp_demo.py --port 18086 --binary path/to/pulsetrader
"""

import argparse
import json
import selectors
import socket
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DEMO_ENGINE = REPO / "tools" / "demo_engine.py"
DEFAULT_BINARY = REPO / "build_headless" / "apps" / "pulsetrader" / "pulsetrader"

# 2026-07-28 is the modern MCP revision: no initialize handshake, and every
# request declares the revision that governs it in `_meta`.
PROTOCOL_VERSION = "2026-07-28"

CALLS = [
    "get_status",
    "get_positions",
    "get_orders",
    "get_signals",
]


def wait_for_port(port: int, timeout: float = 5.0) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return True
        except OSError:
            time.sleep(0.1)
    return False


def request(request_id: int, method: str, params=None) -> dict:
    payload = {
        "jsonrpc": "2.0",
        "id": request_id,
        "method": method,
        "params": params or {},
        "_meta": {"io.modelcontextprotocol/protocolVersion": PROTOCOL_VERSION},
    }
    return payload


def read_line(selector, stream, timeout: float = 5.0) -> str:
    events = selector.select(timeout)
    if not events:
        return ""
    return stream.readline()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", type=int, default=18086)
    parser.add_argument("--binary", default=str(DEFAULT_BINARY))
    parser.add_argument("--json", action="store_true", help="print raw JSON")
    args = parser.parse_args()

    if not Path(args.binary).exists():
        print(f"error: binary not found: {args.binary}", file=sys.stderr)
        print("build it first: cmake --build build_headless", file=sys.stderr)
        return 1

    engine = subprocess.Popen(
        [sys.executable, "-I", str(DEMO_ENGINE), str(args.port)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    bridge = None
    try:
        if not wait_for_port(args.port):
            print("error: demo engine did not start", file=sys.stderr)
            return 1

        bridge = subprocess.Popen(
            [args.binary, "mcp", "--host", "127.0.0.1", "--port", str(args.port)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            text=True, bufsize=1,
        )

        selector = selectors.DefaultSelector()
        selector.register(bridge.stdout, selectors.EVENT_READ)

        next_id = 1

        def exchange(label: str, method: str, params=None) -> dict:
            nonlocal next_id
            message = request(next_id, method, params)
            next_id += 1
            if args.json:
                print(">", json.dumps(message))
            else:
                print(f"\n$ {label}")
            bridge.stdin.write(json.dumps(message) + "\n")
            bridge.stdin.flush()
            line = read_line(selector, bridge.stdout)
            if not line:
                raise RuntimeError(f"no reply to {method}")
            reply = json.loads(line)
            if args.json:
                print("<", line.strip())
            return reply

        # 1. Modern entry point: what can this server speak?
        discovered = exchange("server/discover", "server/discover")
        versions = discovered.get("result", {}).get("supportedVersions", [])
        if not args.json:
            print("  supportedVersions:", versions)

        # 2. Tool catalogue.
        listed = exchange("tools/list", "tools/list")
        tools = listed.get("result", {}).get("tools", [])
        if not args.json:
            names = [t.get("name", "?") for t in tools]
            print(f"  {len(names)} tools: {', '.join(names[:6])} …")

        # 3. Call the read-only tools.
        for method in CALLS:
            reply = exchange(
                f"tools/call {method}",
                "tools/call",
                {"name": method, "arguments": {}},
            )
            if args.json:
                continue
            result = reply.get("result", {})
            content = result.get("content", [])
            text = ""
            if content and isinstance(content, list):
                text = content[0].get("text", "") if isinstance(content[0], dict) else ""
            if text:
                try:
                    parsed = json.loads(text)
                    preview = json.dumps(parsed)[:160]
                except json.JSONDecodeError:
                    preview = text[:160]
                print(f"  {preview}")
            elif "error" in reply:
                print(f"  error: {reply['error']}")

        print()
        return 0
    finally:
        if bridge is not None:
            try:
                bridge.stdin.close()
            except OSError:
                pass
            try:
                bridge.wait(timeout=3)
            except subprocess.TimeoutExpired:
                bridge.kill()
        engine.terminate()
        try:
            engine.wait(timeout=3)
        except subprocess.TimeoutExpired:
            engine.kill()


if __name__ == "__main__":
    sys.exit(main())
