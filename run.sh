#!/bin/bash
# run.sh — pulseTrader launcher script
#
# Usage:
#   ./run.sh trade      Start trading engine (all 9 layers)
#   ./run.sh rest       Test REST API (public + private endpoints)
#   ./run.sh ws         Test WebSocket (market data + private channels)
#   ./run.sh market     Test market data pipeline
#   ./run.sh strategy   Test strategy engine
#   ./run.sh ai         Test AI Pipeline (--mock mode)
#   ./run.sh webui      Start WebUI server
#   ./run.sh test       Run all unit tests

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Load environment variables
if [ -f .env ]; then
    source .env
else
    echo "⚠️  .env file not found, API Key not loaded"
    echo "   Create .env with GATE_API_KEY and GATE_API_SECRET"
fi

BUILD_DIR="build"

usage() {
    echo "Usage: $0 {trade|rest|ws|market|strategy|ai|webui|test} [args...]"
    echo ""
    echo "  trade [--config <path>]  Start trading engine (optional TOML config)"
    echo "  rest      Test Gate.io REST API"
    echo "  ws        Test Gate.io WebSocket"
    echo "  market    Test market data pipeline"
    echo "  strategy  Test strategy engine"
    echo "  ai        Test AI Pipeline (mock)"
    echo "  webui     Start WebUI server"
    echo "  test      Run all unit tests"
    exit 1
}

if [ $# -eq 0 ]; then
    usage
fi

case "$1" in
    trade)
        echo "=== pulseTrader Trading Engine ==="
        shift  # consume 'trade' argument
        # Auto-load trading.toml if no --config specified
        if [ $# -eq 0 ] && [ -f trading.toml ]; then
            echo "📄 Auto-loading trading.toml"
            "$BUILD_DIR/apps/pulsetrader/pulsetrader" --config trading.toml
        else
            "$BUILD_DIR/apps/pulsetrader/pulsetrader" "$@"
        fi
        ;;
    rest)
        echo "=== Gate.io REST API ==="
        "$BUILD_DIR/tools/test_gate_rest"
        ;;
    ws)
        echo "=== Gate.io WebSocket ==="
        "$BUILD_DIR/tools/test_gate_ws"
        ;;
    market)
        echo "=== Market Data Pipeline ==="
        "$BUILD_DIR/tools/test_market_feed"
        ;;
    strategy)
        echo "=== Strategy Engine ==="
        "$BUILD_DIR/tools/test_strategy"
        ;;
    ai)
        echo "=== AI Pipeline (mock) ==="
        "$BUILD_DIR/tools/test_ai_pipeline" --mock
        ;;
    webui)
        echo "=== WebUI Server ==="
        "$BUILD_DIR/tools/test_webui_server"
        ;;
    test)
        echo "=== Running all unit tests ==="
        cd "$BUILD_DIR" && ctest --output-on-failure
        ;;
    *)
        usage
        ;;
esac
