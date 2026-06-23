// test_webui.cpp — Smoke test for WebUI dashboard (Layer 9)
//
// Creates mock components with pre-populated data, launches the
// WebServer on port 8080, and serves the frontend SPA.
//
// Usage: ./build/tools/test_webui_server
// Open browser to http://127.0.0.1:8080?token=demo
// Press Enter to stop.

#include "ai/ai_pipeline.hpp"
#include "core/config.hpp"
#include "exchange/gate_rest_client.hpp"
#include "exchange/gate_ws_client.hpp"
#include "execution/order_tracker.hpp"
#include "logging/logger.hpp"
#include "market/kline_buffer.hpp"
#include "market/market_feed.hpp"
#include "market/orderbook_manager.hpp"
#include "market/ticker_cache.hpp"
#include "risk/drawdown_guard.hpp"
#include "risk/order_rate_limiter.hpp"
#include "risk/position_manager.hpp"
#include "risk/risk_manager.hpp"
#include "strategy/scalping/momentum_scalper.hpp"
#include "strategy/scalping/orderbook_scalper.hpp"
#include "strategy/strategy_manager.hpp"
#include "webui/dashboard_state.hpp"
#include "webui/web_server.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

using namespace pulse;
using namespace pulse::ai;
using namespace pulse::exchange;
using namespace pulse::execution;
using namespace pulse::logging;
using namespace pulse::market;
using namespace pulse::risk;
using namespace pulse::strategy;
using namespace pulse::webui;

// ---------------------------------------------------------------------------
// Mock HTTP transport for AiPipeline — never called in this smoke test
// ---------------------------------------------------------------------------
static AIClient::HttpTransport make_mock_transport()
{
    return [](const std::string &, const std::string &,
              const std::vector<std::string> &) -> Result<nlohmann::json>
    {
        return nlohmann::json{
            {"content", {{{"type", "text"}, {"text", "{}"}}}},
        };
    };
}

// ---------------------------------------------------------------------------
// Populate ticker cache with mock prices
// ---------------------------------------------------------------------------
static void populate_tickers(TickerCache &cache)
{
    Ticker btc;
    btc.symbol = "BTC_USDT";
    btc.last = 65'432.10;
    btc.bid = 65'431.50;
    btc.ask = 65'432.70;
    btc.volume_24h = 12'345.67;
    btc.change_pct = 2.34;
    btc.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    cache.update("BTC_USDT", btc);

    Ticker eth;
    eth.symbol = "ETH_USDT";
    eth.last = 3'456.78;
    eth.bid = 3'456.50;
    eth.ask = 3'457.00;
    eth.volume_24h = 89'012.34;
    eth.change_pct = -1.23;
    eth.timestamp = btc.timestamp;
    cache.update("ETH_USDT", eth);
}

// ---------------------------------------------------------------------------
// Populate order book with mock depth
// ---------------------------------------------------------------------------
static void populate_orderbook(OrderBookManager &mgr)
{
    // BTC_USDT order book
    nlohmann::json btc_snap = {
        {"lastUpdateId", 1000},
        {"bids", nlohmann::json::array({
            nlohmann::json::array({65431.50, 0.150}),
            nlohmann::json::array({65430.00, 0.250}),
            nlohmann::json::array({65428.50, 0.500}),
            nlohmann::json::array({65425.00, 1.200}),
            nlohmann::json::array({65420.00, 2.000}),
            nlohmann::json::array({65415.00, 0.800}),
            nlohmann::json::array({65410.00, 1.500}),
            nlohmann::json::array({65405.00, 0.300}),
            nlohmann::json::array({65400.00, 3.000}),
            nlohmann::json::array({65395.00, 0.600}),
        })},
        {"asks", nlohmann::json::array({
            nlohmann::json::array({65432.70, 0.120}),
            nlohmann::json::array({65434.00, 0.300}),
            nlohmann::json::array({65436.50, 0.450}),
            nlohmann::json::array({65440.00, 1.100}),
            nlohmann::json::array({65445.00, 1.800}),
            nlohmann::json::array({65450.00, 0.900}),
            nlohmann::json::array({65455.00, 1.200}),
            nlohmann::json::array({65460.00, 0.500}),
            nlohmann::json::array({65465.00, 2.500}),
            nlohmann::json::array({65470.00, 0.700}),
        })},
    };
    mgr.applySnapshot("BTC_USDT", btc_snap);

    // ETH_USDT order book
    nlohmann::json eth_snap = {
        {"lastUpdateId", 2000},
        {"bids", nlohmann::json::array({
            nlohmann::json::array({3456.50, 2.50}),
            nlohmann::json::array({3456.00, 5.00}),
            nlohmann::json::array({3455.50, 8.00}),
            nlohmann::json::array({3455.00, 12.00}),
            nlohmann::json::array({3454.00, 20.00}),
            nlohmann::json::array({3453.00, 6.00}),
            nlohmann::json::array({3452.00, 10.00}),
            nlohmann::json::array({3451.00, 4.00}),
            nlohmann::json::array({3450.00, 30.00}),
            nlohmann::json::array({3449.00, 8.00}),
        })},
        {"asks", nlohmann::json::array({
            nlohmann::json::array({3457.00, 3.00}),
            nlohmann::json::array({3457.50, 4.50}),
            nlohmann::json::array({3458.00, 6.00}),
            nlohmann::json::array({3458.50, 10.00}),
            nlohmann::json::array({3459.00, 15.00}),
            nlohmann::json::array({3460.00, 7.00}),
            nlohmann::json::array({3461.00, 9.00}),
            nlohmann::json::array({3462.00, 5.00}),
            nlohmann::json::array({3463.00, 25.00}),
            nlohmann::json::array({3464.00, 10.00}),
        })},
    };
    mgr.applySnapshot("ETH_USDT", eth_snap);
}

// ---------------------------------------------------------------------------
// Populate kline buffer with mock candles
// ---------------------------------------------------------------------------
static void populate_klines(MarketFeed &feed)
{
    auto &buf = feed.getKlineBuffer("BTC_USDT");

    auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();

    for (int i = 19; i >= 0; --i)
    {
        Kline k;
        k.open_time = nowMs - (i * 60'000); // 1-minute candles
        k.close_time = k.open_time + 59'999;
        k.open = 65'000.0 + (i % 5) * 100.0 - (i % 3) * 50.0;
        k.high = k.open + 150.0 + (i % 4) * 30.0;
        k.low = k.open - 100.0 - (i % 3) * 20.0;
        k.close = k.open + (i % 2 == 0 ? 80.0 : -40.0);
        k.volume = 50.0 + (i % 7) * 10.0;
        k.closed = true;
        buf.push(k);
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main()
{
    // 1. Initialize logging (console only, info level)
    LogConfig log_cfg;
    log_cfg.level = "info";
    log_cfg.toConsole = true;
    log_cfg.toFile = false;
    Logger::init(log_cfg);

    std::cout << "=== pulseTrader WebUI Smoke Test ===" << std::endl;
    std::cout << std::endl;

    // 2. Create mock exchange clients (never started — no network I/O)
    ExchangeConfig exchange_cfg;
    GateWsClient ws_client(exchange_cfg);
    GateRestClient rest_client(exchange_cfg);

    // 3. Create and populate MarketFeed components
    MarketFeed feed(ws_client, rest_client);
    populate_tickers(feed.tickerCache());
    populate_orderbook(feed.orderbookManager());
    populate_klines(feed);

    std::cout << "[OK] MarketFeed populated with mock data" << std::endl;

    // 4. Create PositionManager and open mock positions
    RiskConfig risk_cfg;
    risk_cfg.maxPositionNotional = 10'000.0;
    risk_cfg.maxOpenPositions = 10;
    risk_cfg.maxSymbolNotional = 5'000.0;

    PositionManager position_mgr(risk_cfg);

    auto pos1 = position_mgr.openPosition(
        "BTC_USDT", Side::Buy, 0.015, 65'200.0, "momentum_scalper_BTC_USDT");
    if (ok(pos1))
    {
        position_mgr.updatePrice(value(pos1), 65'432.10);
    }

    auto pos2 = position_mgr.openPosition(
        "ETH_USDT", Side::Sell, 1.0, 3'500.0, "orderbook_scalper_ETH_USDT");
    if (ok(pos2))
    {
        position_mgr.updatePrice(value(pos2), 3'456.78);
    }

    std::cout << "[OK] PositionManager: " << position_mgr.getAllPositions().size()
              << " positions opened" << std::endl;

    // 5. Create StrategyManager and register strategies
    StrategyManager strategy_mgr;

    StrategyContext ctx1;
    ctx1.config.name = "momentum_scalper";
    ctx1.config.symbol = "BTC_USDT";
    ctx1.config.order_quantity = 0.001;
    ctx1.config.min_confidence = 0.6;
    ctx1.config.enabled = true;
    ctx1.config.poll_interval_ms = 500;
    strategy_mgr.registerStrategy(std::make_unique<MomentumScalper>(ctx1));

    StrategyContext ctx2;
    ctx2.config.name = "orderbook_scalper";
    ctx2.config.symbol = "ETH_USDT";
    ctx2.config.order_quantity = 0.01;
    ctx2.config.min_confidence = 0.5;
    ctx2.config.enabled = true;
    ctx2.config.poll_interval_ms = 200;
    strategy_mgr.registerStrategy(std::make_unique<OrderBookScalper>(ctx2));

    std::cout << "[OK] StrategyManager: " << strategy_mgr.strategyCount()
              << " strategies registered" << std::endl;

    // 6. Create DrawdownGuard, OrderRateLimiter, RiskManager
    DrawdownGuard drawdownGuard(risk_cfg);
    drawdownGuard.updateEquity(10'000.0); // Set initial equity

    OrderRateLimiter rateLimiter(risk_cfg.maxOrdersPerSec);

    RiskManager risk_mgr(risk_cfg, position_mgr, drawdownGuard, rateLimiter);

    std::cout << "[OK] RiskManager initialized" << std::endl;

    // 7. Create OrderTracker (with mock clients — never started)
    OrderTracker order_tracker(ws_client, rest_client);

    std::cout << "[OK] OrderTracker initialized" << std::endl;

    // 8. Create AiPipeline (with mock transport)
    AiConfig ai_cfg;
    ai_cfg.backend = "claude";
    ai_cfg.model = "mock";
    ai_cfg.maxRetries = 0;

    TwitterConfig tw_cfg;
    tw_cfg.enabled = false;

    NewsConfig news_cfg;
    news_cfg.enabled = false;

    AiPipeline ai_pipeline(ai_cfg, tw_cfg, news_cfg, make_mock_transport());

    std::cout << "[OK] AiPipeline initialized (mock transport)" << std::endl;

    // 9. Create DashboardState, wire all components
    WebUiConfig webui_cfg;
    webui_cfg.enabled = true;
    webui_cfg.bindAddress = "127.0.0.1";
    webui_cfg.port = 8080;
    webui_cfg.authToken = "demo";
    webui_cfg.maxClients = 4;

    DashboardState state(webui_cfg, feed, strategy_mgr, risk_mgr,
                         order_tracker, ai_pipeline);

    // 10. Create WebServer
    WebServer server(webui_cfg, state, "frontend");

    // 11. Wire snapshot callback: DashboardState → WsServer
    //     NOTE: wsServer() returns const ref but pushSnapshot() is non-const.
    //     This const_cast is safe: pushSnapshot() only modifies mutex-protected
    //     internal state. A non-const accessor should be added to WebServer.
    auto &ws_ref = const_cast<WsServer &>(server.wsServer());
    state.setSnapshotCallback([&ws_ref](std::shared_ptr<const DashboardSnapshot> snap)
        {
            ws_ref.pushSnapshot(snap);
        });

    // 12. Start DashboardState polling
    state.start();
    std::cout << "[OK] DashboardState polling started" << std::endl;

    // 13. Start WebServer
    if (!server.start())
    {
        std::cerr << "[FAIL] WebServer failed to start on port " << webui_cfg.port << std::endl;
        state.stop();
        Logger::shutdown();
        return 1;
    }

    std::cout << "[OK] WebServer started on port " << server.port() << std::endl;
    std::cout << std::endl;

    // 14. Print usage instructions
    std::cout << "========================================" << std::endl;
    std::cout << " Dashboard running at http://127.0.0.1:" << server.port() << std::endl;
    std::cout << " Auth token: " << webui_cfg.authToken << std::endl;
    std::cout << " Open browser to:" << std::endl;
    std::cout << "   http://127.0.0.1:" << server.port() << "?token=" << webui_cfg.authToken << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;
    std::cout << "Press Enter to stop..." << std::endl;

    // 15. Block until user presses Enter
    std::cin.get();

    // 16. Stop all components
    std::cout << std::endl;
    std::cout << "[INFO] Stopping WebServer..." << std::endl;
    server.stop();

    std::cout << "[INFO] Stopping DashboardState..." << std::endl;
    state.stop();

    std::cout << "[INFO] Stopping StrategyManager..." << std::endl;
    strategy_mgr.stop();

    std::cout << "[INFO] Done" << std::endl;

    Logger::shutdown();
    return 0;
}
