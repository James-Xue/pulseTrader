#!/bin/bash
# run.sh — pulseTrader 快捷运行脚本
#
# Usage:
#   ./run.sh trade      启动交易主程序（9层全量运行）
#   ./run.sh rest       测试 REST API（公开 + 私有接口）
#   ./run.sh ws         测试 WebSocket（行情 + 私有频道）
#   ./run.sh market     测试行情数据管道
#   ./run.sh strategy   测试策略引擎
#   ./run.sh ai         测试 AI Pipeline（--mock 模式）
#   ./run.sh webui      启动 WebUI 服务器
#   ./run.sh test       运行全部单元测试

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# 加载环境变量
if [ -f .env ]; then
    source .env
else
    echo "⚠️  .env 文件不存在，API Key 未加载"
    echo "   创建 .env 并填入 GATE_API_KEY 和 GATE_API_SECRET"
fi

BUILD_DIR="build"

usage() {
    echo "Usage: $0 {trade|rest|ws|market|strategy|ai|webui|test} [args...]"
    echo ""
    echo "  trade [--config <path>]  启动交易主程序（可选 TOML 配置文件）"
    echo "  rest      测试 Gate.io REST API"
    echo "  ws        测试 Gate.io WebSocket"
    echo "  market    测试行情数据管道"
    echo "  strategy  测试策略引擎"
    echo "  ai        测试 AI Pipeline (mock)"
    echo "  webui     启动 WebUI 服务器"
    echo "  test      运行全部单元测试"
    exit 1
}

if [ $# -eq 0 ]; then
    usage
fi

case "$1" in
    trade)
        echo "=== pulseTrader Trading Engine ==="
        shift  # consume 'trade' argument
        # 如果没有手动指定 --config，且 trading.toml 存在，自动使用
        if [ $# -eq 0 ] && [ -f trading.toml ]; then
            echo "📄 自动加载 trading.toml"
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
