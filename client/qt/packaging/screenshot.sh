#!/bin/sh
# Regenerate the README screenshots.
#
# The client can render itself to a PNG and exit (--screenshot), so the images
# in docs/images/ are reproducible instead of hand-captured. The demo engine
# supplies sample data so the capture is not an empty window.
#
# Usage: packaging/screenshot.sh [output-dir]

set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
CLIENT_DIR=$(cd "$SCRIPT_DIR/.." && pwd)
REPO_DIR=$(cd "$CLIENT_DIR/../.." && pwd)

BIN=${PULSE_QT_BINARY:-"$REPO_DIR/build_qt/pulseTraderQt"}
OUT=${1:-"$REPO_DIR/docs/images"}
PORT=${PULSE_DEMO_PORT:-18099}
DELAY=${PULSE_SHOT_DELAY:-2000}

if [ ! -x "$BIN" ]; then
    echo "error: client binary not found: $BIN" >&2
    echo "build it first: cmake --build build_qt" >&2
    exit 1
fi

mkdir -p "$OUT"

python3 -I "$REPO_DIR/tools/demo_engine.py" "$PORT" >/dev/null 2>&1 &
DEMO_PID=$!
trap 'kill "$DEMO_PID" 2>/dev/null || true' EXIT INT TERM

# Give the fixture a moment to bind before the client connects.
python3 - "$PORT" <<'PY'
import socket, sys, time
port = int(sys.argv[1])
for _ in range(50):
    try:
        socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
        break
    except OSError:
        time.sleep(0.1)
PY

export QT_QPA_PLATFORM=offscreen
export PULSE_CLIENT_HOST=127.0.0.1
export PULSE_CLIENT_PORT="$PORT"

"$BIN" --screenshot "$OUT/ui-light.png" --light --screenshot-delay "$DELAY"
"$BIN" --screenshot "$OUT/ui-dark.png"  --dark  --screenshot-delay "$DELAY"

echo "wrote:"
echo "  $OUT/ui-light.png"
echo "  $OUT/ui-dark.png"
