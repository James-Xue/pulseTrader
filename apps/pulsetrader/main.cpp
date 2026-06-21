// main.cpp — pulseTrader trading engine entry point
//
// Wires all 9 layers into a single trading process:
//   L1 Exchange  → Gate.io REST + WebSocket
//   L2 Logging   → spdlog async logger
//   L3 Market    → hot-path market data pipeline
//   L4 AI        → LLM-driven parameter adaptation
//   L5 Heartbeat → 5-min AI analysis clock
//   L6 Strategy  → multi-strategy orchestration + signal aggregation
//   L7 Risk      → position/drawdown/rate-limit gate
//   L8 Execution → order placement + lifecycle tracking
//   L9 WebUI     → optional real-time dashboard
//
// Usage:
//   pulsetrader                          Start with default config + .env credentials
//   pulsetrader --config trading.toml    Start with TOML config file
//   pulsetrader --help                   Print usage
//   pulsetrader --version                Print version

#include "core/config.hpp"
#include "core/config_loader.hpp"
#include "core/config_validator.hpp"
#include "core/types.hpp"
#include "exchange/gate_rest_client.hpp"
#include "exchange/gate_ws_client.hpp"
#include "logging/logger.hpp"
#include "market/market_feed.hpp"
#include "risk/drawdown_guard.hpp"
#include "risk/order_rate_limiter.hpp"
#include "risk/position_manager.hpp"
#include "risk/risk_manager.hpp"
#include "execution/order_executor.hpp"
#include "execution/order_tracker.hpp"
#include "strategy/strategy_manager.hpp"
#include "strategy/signal/signal_aggregator.hpp"
#include "strategy/scalping/momentum_scalper.hpp"
#include "strategy/scalping/orderbook_scalper.hpp"
#include "strategy/scalping/mean_reversion_scalper.hpp"
#include "ai/ai_pipeline.hpp"
#include "heartbeat/heartbeat_scheduler.hpp"

#ifdef PULSE_ENABLE_WEBUI
#include "webui/dashboard_state.hpp"
#include "webui/web_server.hpp"
#endif

#ifdef PULSE_ENABLE_SQLITE
#include "trade_recorder/trade_recorder.hpp"
#endif

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Global stop flag — set by SIGINT / SIGTERM handler
// ---------------------------------------------------------------------------
static std::atomic<bool> g_stop_requested{ false };

static void signal_handler(int /*sig*/)
{
    g_stop_requested.store(true, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static std::string env_or(const char* name, const std::string& fallback)
{
    const char* val = std::getenv(name);
    return (val && val[0]) ? std::string(val) : fallback;
}

static void print_usage(const char* prog)
{
    std::cout
        << "pulseTrader v0.1.0 — AI-driven scalping framework\n\n"
        << "Usage:\n"
        << "  " << prog << "                          Start with default config\n"
        << "  " << prog << " --config <path>         Load config from TOML file\n"
        << "  " << prog << " --help                  Print this message\n"
        << "  " << prog << " --version               Print version\n\n"
        << "Environment variables (via .env or shell):\n"
        << "  GATE_API_KEY      Gate.io API key (required without --config)\n"
        << "  GATE_API_SECRET   Gate.io API secret (required without --config)\n"
        << "  HTTPS_PROXY       HTTP proxy for REST + WebSocket\n"
        << "  PULSE_WEBUI_PORT  WebUI listen port (default: 8080)\n"
        << "  PULSE_WEBUI_TOKEN WebUI bearer token (default: pulsetrader)\n\n"
        << "TOML config (--config):\n"
        << "  Use from_env:VAR_NAME syntax to read sensitive values from env.\n"
        << "  Example: apiKey = \"from_env:GATE_API_KEY\"\n"
        << "  See trading.toml.example for a complete template.\n"
        << std::endl;
}

// ---------------------------------------------------------------------------
// Build default PulseConfig with env-var overrides
// ---------------------------------------------------------------------------
static pulse::PulseConfig build_default_config()
{
    using namespace pulse;

    PulseConfig cfg;

    // Detect network mode: "mainnet" (default) or "testnet".
    std::string network = env_or("PULSE_NETWORK", "mainnet");
    bool is_testnet = ("testnet" == network);

    // L1: Exchange — credentials and URLs depend on network mode.
    if (is_testnet)
    {
        cfg.exchange.testnet = true;
        cfg.exchange.apiKey    = env_or("GATE_TESTNET_API_KEY", "");
        cfg.exchange.apiSecret = env_or("GATE_TESTNET_API_SECRET", "");
        cfg.exchange.restBaseUrl   = pulse::url::kTestnetRest;
        cfg.exchange.wsUrl         = pulse::url::kTestnetSpotWs;
        cfg.exchange.futuresWsUrl  = pulse::url::kTestnetFuturesWs;
    }
    else
    {
        // Backward compatible: try GATE_MAINNET_* first, fall back to GATE_*.
        cfg.exchange.apiKey = env_or("GATE_MAINNET_API_KEY",
                                     env_or("GATE_API_KEY", ""));
        cfg.exchange.apiSecret = env_or("GATE_MAINNET_API_SECRET",
                                        env_or("GATE_API_SECRET", ""));
        cfg.exchange.restBaseUrl = env_or("GATE_MAINNET_REST_URL",
                                          "https://api.gateio.ws");
        cfg.exchange.wsUrl = env_or("GATE_MAINNET_SPOT_WS_URL",
                                    "wss://api.gateio.ws/ws/v4/");
        cfg.exchange.futuresWsUrl = env_or("GATE_MAINNET_FUTURES_WS_URL",
                                           "wss://fx-ws.gateio.ws/v4/ws/usdt");
    }

    cfg.exchange.proxyUrl   = env_or("HTTPS_PROXY", env_or("HTTP_PROXY", ""));

    // L2: Logging
    cfg.log.level    = "info";
    cfg.log.logDir   = "logs";
    cfg.log.toConsole = true;
    cfg.log.toFile    = true;

    // Trading symbols
    cfg.symbols = { "BTC_USDT" };

    // L6: Strategy — 2 instances on BTC_USDT
    {
        StrategyInstanceConfig momentum;
        momentum.name            = "momentum_scalper";
        momentum.symbol          = "BTC_USDT";
        momentum.order_quantity  = 0.001;
        momentum.min_confidence  = 0.6;
        momentum.enabled         = true;
        momentum.poll_interval_ms = 500;
        cfg.strategy.strategies.push_back(momentum);

        StrategyInstanceConfig ob;
        ob.name            = "orderbook_scalper";
        ob.symbol          = "BTC_USDT";
        ob.order_quantity  = 0.001;
        ob.min_confidence  = 0.65;
        ob.enabled         = true;
        ob.poll_interval_ms = 200;
        cfg.strategy.strategies.push_back(ob);
    }

    // L6: Aggregator
    cfg.strategy.signal_aggregator_threshold = 0.7;
    cfg.strategy.signal_cooldown_sec         = 30;

    // L7: Risk
    cfg.risk.maxPositionNotional = 500.0;
    cfg.risk.maxOpenPositions    = 3;
    cfg.risk.maxDailyDrawdown    = 0.02;
    cfg.risk.maxDrawdown         = 0.05;
    cfg.risk.maxOrdersPerSec     = 5;
    cfg.risk.maxSymbolNotional   = 300.0;

    // L4: AI — disabled by default (no API key configured)
    cfg.ai.backend              = "openai";
    cfg.ai.model                = "gpt-4o";
    cfg.ai.apiKey               = env_or("OPENAI_API_KEY", "");
    cfg.ai.heartbeatIntervalSec = 0;  // Disabled until AI key is provided.
    cfg.ai.requestTimeoutMs     = 30'000;

    // L9: WebUI
    cfg.webui.enabled      = true;
    cfg.webui.bindAddress  = "127.0.0.1";
    cfg.webui.port         = static_cast<std::uint16_t>(
        std::stoi(env_or("PULSE_WEBUI_PORT", "8080")));
    cfg.webui.authToken    = env_or("PULSE_WEBUI_TOKEN", "pulsetrader");
    cfg.webui.maxClients   = 4;

    return cfg;
}

// ---------------------------------------------------------------------------
// Create a strategy instance from config
// ---------------------------------------------------------------------------
static std::unique_ptr<pulse::strategy::StrategyBase>
create_strategy(const std::string& name,
                const pulse::strategy::StrategyContext& ctx)
{
    using namespace pulse::strategy;

    if ("momentum_scalper" == name)
    {
        return std::make_unique<MomentumScalper>(ctx);
    }
    if ("orderbook_scalper" == name)
    {
        return std::make_unique<OrderBookScalper>(ctx);
    }
    if ("mean_reversion_scalper" == name)
    {
        return std::make_unique<MeanReversionScalper>(ctx);
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// log_system_heartbeat — periodic system health summary
//
// Logs a single compact line every 60 seconds showing:
//   - Process uptime (human-readable)
//   - Market data rates (events/sec, delta since last call)
//   - WebSocket connection states
//   - Strategy thread activity
//   - Open position count and notional exposure
//
// Thread safety:
//   Called only from the main thread. All accessed methods are thread-safe
//   (atomic loads, shared_mutex reads).
// ---------------------------------------------------------------------------
static void log_system_heartbeat(
    std::chrono::steady_clock::time_point start_time,
    const pulse::market::MarketFeed* spot_feed,
    const pulse::market::MarketFeed* futures_feed,
    const pulse::exchange::GateWsClient* spot_ws,
    const pulse::exchange::GateWsClient* futures_ws,
    const pulse::strategy::StrategyManager& strategy_mgr,
    const pulse::risk::PositionManager& position_mgr)
{
    // --- Uptime formatting ---
    const auto elapsed = std::chrono::steady_clock::now() - start_time;
    const auto total_sec = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
    const auto hours   = total_sec / 3600;
    const auto minutes = (total_sec % 3600) / 60;
    const auto seconds = total_sec % 60;

    std::ostringstream oss;

    if (0 < hours)
    {
        oss << hours << "h" << std::setfill('0') << std::setw(2) << minutes << "m";
    }
    else if (0 < minutes)
    {
        oss << minutes << "m" << std::setfill('0') << std::setw(2) << seconds << "s";
    }
    else
    {
        oss << seconds << "s";
    }

    const std::string uptime_str = oss.str();

    // --- Market data rates (delta over 60s interval) ---
    // Static locals persist previous readings for delta computation.
    // These are only accessed from the main thread, so no race.
    static pulse::market::FeedStats prev_spot    = { 0, 0, 0 };
    static pulse::market::FeedStats prev_futures = { 0, 0, 0 };
    static bool first_call = true;

    // Format a market feed section: "100 tick/s  10 kline/s  80 ob/s"
    auto format_feed = [&](const pulse::market::MarketFeed* feed,
                           pulse::market::FeedStats& prev,
                           std::ostringstream& out)
    {
        if (nullptr == feed)
        {
            return;
        }

        const auto cur = feed->stats();

        if (first_call)
        {
            // First heartbeat: no delta available, show cumulative.
            prev = cur;
            out << cur.ticker_count << " tick  "
                << cur.kline_count << " kline  "
                << cur.orderbook_count << " ob (init)";
            return;
        }

        // Delta rate over 60-second interval.
        constexpr double kIntervalSec = 60.0;
        const auto tick_d  = cur.ticker_count    - prev.ticker_count;
        const auto kline_d = cur.kline_count     - prev.kline_count;
        const auto ob_d    = cur.orderbook_count - prev.orderbook_count;
        prev = cur;

        out << static_cast<std::uint64_t>(tick_d / kIntervalSec) << " tick/s  "
            << static_cast<std::uint64_t>(kline_d / kIntervalSec) << " kline/s  "
            << static_cast<std::uint64_t>(ob_d / kIntervalSec) << " ob/s";
    };

    // --- WS connection state labels ---
    auto ws_label = [](const pulse::exchange::GateWsClient* ws) -> const char*
    {
        if (nullptr == ws)
        {
            return "n/a";
        }

        switch (ws->state())
        {
            case pulse::exchange::WsConnectionState::Connected:    return "connected";
            case pulse::exchange::WsConnectionState::Connecting:   return "connecting";
            case pulse::exchange::WsConnectionState::Disconnected: return "disconnected";
        }
        return "unknown";
    };

    // --- Build the log message ---
    std::ostringstream msg;
    msg << "[heartbeat] uptime " << uptime_str;

    // Spot section (omitted if no spot feed).
    if (nullptr != spot_feed)
    {
        msg << " | spot ";
        format_feed(spot_feed, prev_spot, msg);
    }

    // Futures section (omitted if no futures feed).
    if (nullptr != futures_feed)
    {
        msg << " | futures ";
        format_feed(futures_feed, prev_futures, msg);
    }

    // WS status.
    msg << " | ws spot=" << ws_label(spot_ws)
        << " futures=" << ws_label(futures_ws);

    // Strategy status.
    msg << " | strategies " << strategy_mgr.running_count()
        << "/" << strategy_mgr.strategy_count() << " running";

    // Position status.
    const auto portfolio = position_mgr.portfolio_summary();
    msg << " | positions " << position_mgr.open_position_count()
        << " (notional " << std::fixed << std::setprecision(2)
        << portfolio.total_notional << " USDT)";

    PULSE_LOG_INFO("system", "{}", msg.str());

    first_call = false;
}

// ===========================================================================
// main
// ===========================================================================
int main(int argc, char* argv[])
{
    using pulse::MarketType;
    using pulse::MarginMode;

    // ------------------------------------------------------------------
    // 0. Command-line parsing
    // ------------------------------------------------------------------
    std::string config_path;

    for (int i = 1; i < argc; ++i)
    {
        std::string arg(argv[i]);

        if ("--help" == arg || "-h" == arg)
        {
            print_usage(argv[0]);
            return 0;
        }

        if ("--version" == arg || "-v" == arg)
        {
            std::cout << "pulseTrader v0.1.0" << std::endl;
            return 0;
        }

        if ("--config" == arg)
        {
            if (i + 1 >= argc)
            {
                std::cerr << "Error: --config requires a file path argument.\n";
                return 1;
            }

            config_path = argv[++i];
            continue;
        }

        std::cerr << "Unknown argument: " << arg << "\n";
        print_usage(argv[0]);
        return 1;
    }

    // ------------------------------------------------------------------
    // 1. Build configuration
    // ------------------------------------------------------------------
    pulse::PulseConfig cfg;

    if (!config_path.empty())
    {
        // Load from TOML file.
        auto result = pulse::load_config_file(config_path);

        if (!pulse::ok(result))
        {
            std::cerr << "Config error: "
                      << pulse::error(result).message << "\n";
            return 1;
        }

        cfg = pulse::value(result);

        // Note: testnet URL switching is handled by config_loader::parse_exchange()
        // which reads testnet flag first and sets URL defaults accordingly.
    }
    else
    {
        cfg = build_default_config();
    }

    // Validate regardless of source.
    auto validation_err = pulse::validate_config(cfg);

    if (pulse::ErrorCode::Ok != validation_err.code)
    {
        std::cerr << "Config validation: " << validation_err.message << "\n";
        return 1;
    }

    if (cfg.exchange.apiKey.empty() || cfg.exchange.apiSecret.empty())
    {
        std::cerr << "Error: exchange.apiKey and exchange.apiSecret must be set.\n";

        if (config_path.empty())
        {
            if (cfg.exchange.testnet)
            {
                std::cerr << "  Set PULSE_NETWORK=testnet and GATE_TESTNET_API_KEY / GATE_TESTNET_API_SECRET in .env\n";
            }
            else
            {
                std::cerr << "  source .env  or  export GATE_MAINNET_API_KEY=... GATE_MAINNET_API_SECRET=...\n";
            }
        }
        else
        {
            std::cerr << "  Use from_env:GATE_MAINNET_API_KEY / from_env:GATE_MAINNET_API_SECRET in TOML.\n";
        }

        return 1;
    }

    // ------------------------------------------------------------------
    // 2. L2: Logging (must be first)
    // ------------------------------------------------------------------
    pulse::logging::Logger::init(cfg.log);
    auto log = pulse::logging::Logger::get("app");

    log->info("pulseTrader v0.1.0 starting...");
    log->info("Exchange: Gate.io (REST + WS)");
    log->info("Symbols:  {}", fmt::join(cfg.symbols, ", "));

    if (cfg.exchange.testnet)
    {
        log->warn("========================================");
        log->warn("TESTNET MODE — using virtual funds");
        log->warn("REST:        {}", cfg.exchange.restBaseUrl);
        log->warn("Spot WS:     {}", cfg.exchange.wsUrl);
        log->warn("Futures WS:  {}", cfg.exchange.futuresWsUrl);
        log->warn("========================================");
    }

    if (!cfg.exchange.proxyUrl.empty())
    {
        log->info("Proxy:    {}", cfg.exchange.proxyUrl);
    }

    // ------------------------------------------------------------------
    // 3. L1: Exchange clients (dual-market support)
    // ------------------------------------------------------------------
    // Detect which market types are needed by enabled strategies.
    bool has_spot = false;
    bool has_futures = false;
    for (const auto& inst : cfg.strategy.strategies)
    {
        if (!inst.enabled) continue;
        if (MarketType::Futures == inst.market_type) has_futures = true;
        else has_spot = true;
    }
    // Default to spot if no strategies configured (backward compatibility).
    if (!has_spot && !has_futures) has_spot = true;

    // Spot infrastructure.
    std::unique_ptr<pulse::exchange::GateRestClient> spot_rest;
    std::unique_ptr<pulse::exchange::GateWsClient>   spot_ws;
    std::unique_ptr<pulse::market::MarketFeed>       spot_feed;
    std::unique_ptr<pulse::execution::OrderExecutor>  spot_executor;
    std::unique_ptr<pulse::execution::OrderTracker>   spot_tracker;

    // Futures infrastructure.
    std::unique_ptr<pulse::exchange::GateRestClient> futures_rest;
    std::unique_ptr<pulse::exchange::GateWsClient>   futures_ws;
    std::unique_ptr<pulse::market::MarketFeed>       futures_feed;
    std::unique_ptr<pulse::execution::OrderExecutor>  futures_executor;
    std::unique_ptr<pulse::execution::OrderTracker>   futures_tracker;

    if (has_spot)
    {
        spot_rest = std::make_unique<pulse::exchange::GateRestClient>(
            cfg.exchange, MarketType::Spot);
        spot_ws = std::make_unique<pulse::exchange::GateWsClient>(
            cfg.exchange, MarketType::Spot);
        spot_feed = std::make_unique<pulse::market::MarketFeed>(
            *spot_ws, *spot_rest, MarketType::Spot);
        spot_executor = std::make_unique<pulse::execution::OrderExecutor>(
            *spot_rest, MarketType::Spot);
        spot_tracker = std::make_unique<pulse::execution::OrderTracker>(
            *spot_ws, *spot_rest, MarketType::Spot);
        log->info("[L1] Spot exchange clients created");
    }

    if (has_futures)
    {
        futures_rest = std::make_unique<pulse::exchange::GateRestClient>(
            cfg.exchange, MarketType::Futures);
        futures_ws = std::make_unique<pulse::exchange::GateWsClient>(
            cfg.exchange, MarketType::Futures);
        futures_feed = std::make_unique<pulse::market::MarketFeed>(
            *futures_ws, *futures_rest, MarketType::Futures);
        futures_executor = std::make_unique<pulse::execution::OrderExecutor>(
            *futures_rest, MarketType::Futures);
        futures_tracker = std::make_unique<pulse::execution::OrderTracker>(
            *futures_ws, *futures_rest, MarketType::Futures);
        log->info("[L1] Futures exchange clients created");
    }

    // ------------------------------------------------------------------
    // 4. L3: Market Data (per-market feeds already created above)
    // ------------------------------------------------------------------
    log->info("[L3] Market feed(s) ready (spot={}, futures={})",
              has_spot ? "yes" : "no", has_futures ? "yes" : "no");

    // ------------------------------------------------------------------
    // 5. L7: Risk Management
    // ------------------------------------------------------------------
    pulse::risk::PositionManager   position_mgr(cfg.risk);
    pulse::risk::DrawdownGuard     drawdown_guard(cfg.risk);
    pulse::risk::OrderRateLimiter  rate_limiter(cfg.risk.maxOrdersPerSec);
    pulse::risk::RiskManager       risk_mgr(cfg.risk, position_mgr,
                                            drawdown_guard, rate_limiter);

    log->info("[L7] Risk manager created (max notional: {} USDT, "
              "daily DD limit: {:.1f}%)",
              cfg.risk.maxPositionNotional, cfg.risk.maxDailyDrawdown * 100);

    // ------------------------------------------------------------------
    // 6. L8: Order Execution (per-market executors already created above)
    // ------------------------------------------------------------------
    log->info("[L8] Order executor + tracker ready");

    // ------------------------------------------------------------------
    // 6b. Trade Recorder (optional, Phase 2)
    // ------------------------------------------------------------------
#ifdef PULSE_ENABLE_SQLITE
    std::unique_ptr<pulse::trade_recorder::TradeRecorder> trade_recorder;

    if (cfg.sqlite.enabled)
    {
        // Ensure parent directory exists.
        std::filesystem::path db_dir =
            std::filesystem::path(cfg.sqlite.dbPath).parent_path();
        if (!db_dir.empty())
        {
            std::filesystem::create_directories(db_dir);
        }

        auto rec_result = pulse::trade_recorder::TradeRecorder::open(
            cfg.sqlite.dbPath);

        if (pulse::ok(rec_result))
        {
            trade_recorder = std::make_unique<
                pulse::trade_recorder::TradeRecorder>(
                    std::move(pulse::value(rec_result)));
            log->info("[L8+] Trade recorder opened: '{}'",
                      cfg.sqlite.dbPath);
        }
        else
        {
            log->warn("[L8+] Trade recorder failed to open: {}",
                      pulse::error(rec_result).message);
        }
    }
    else
    {
        log->info("[L8+] Trade recorder disabled "
                  "(set sqlite.enabled = true)");
    }
#else
    log->info("[L8+] Trade recorder disabled "
              "(compile with -DPULSE_ENABLE_SQLITE=ON)");
#endif

    // ------------------------------------------------------------------
    // 7. L6: Strategy Engine
    // ------------------------------------------------------------------
    pulse::strategy::StrategyManager strategy_mgr;
    pulse::strategy::SignalAggregator aggregator(cfg.strategy);

    // Register strategy instances from config.
    for (const auto& inst_cfg : cfg.strategy.strategies)
    {
        if (!inst_cfg.enabled)
        {
            continue;
        }

        // Select the correct MarketFeed and OrderExecutor for this strategy's market.
        pulse::market::MarketFeed* feed_ptr = nullptr;
        pulse::execution::OrderExecutor* exec_ptr = nullptr;

        if (MarketType::Futures == inst_cfg.market_type && futures_feed)
        {
            feed_ptr = futures_feed.get();
            exec_ptr = futures_executor.get();
        }
        else if (spot_feed)
        {
            feed_ptr = spot_feed.get();
            exec_ptr = spot_executor.get();
        }

        if (!feed_ptr || !exec_ptr)
        {
            log->warn("No {} infrastructure available for strategy '{}', skipping",
                      MarketType::Futures == inst_cfg.market_type ? "futures" : "spot",
                      inst_cfg.name);
            continue;
        }

        pulse::strategy::StrategyContext ctx(*feed_ptr, risk_mgr,
                                             *exec_ptr, inst_cfg);

        auto strat = create_strategy(inst_cfg.name, ctx);
        if (!strat)
        {
            log->warn("Unknown strategy '{}', skipping", inst_cfg.name);
            continue;
        }

        log->info("[L6] Registered strategy: {} on {} (qty={}, conf={:.2f}, market={})",
                  inst_cfg.name, inst_cfg.symbol,
                  inst_cfg.order_quantity, inst_cfg.min_confidence,
                  MarketType::Futures == inst_cfg.market_type ? "futures" : "spot");

        strategy_mgr.register_strategy(std::move(strat));
    }

    if (0 == strategy_mgr.strategy_count())
    {
        log->error("No strategies registered. Exiting.");
        pulse::logging::Logger::shutdown();
        return 1;
    }

    // Wire: strategy signals → aggregator
    strategy_mgr.set_signal_callback(
        [&aggregator](const pulse::strategy::TradingSignal& sig)
        {
            aggregator.add_signal(sig);
        });

    log->info("[L6] Strategy engine ready ({} instances, threshold={:.2f}, "
              "cooldown={}s)",
              strategy_mgr.strategy_count(),
              cfg.strategy.signal_aggregator_threshold,
              cfg.strategy.signal_cooldown_sec);

    // ------------------------------------------------------------------
    // 8. L4 + L5: AI Pipeline + Heartbeat
    // ------------------------------------------------------------------
    pulse::ai::AiPipeline ai_pipeline(cfg.ai, cfg.twitter, cfg.news);

    std::unique_ptr<pulse::heartbeat::HeartbeatScheduler> heartbeat;
    if (cfg.ai.heartbeatIntervalSec > 0 && !cfg.ai.apiKey.empty())
    {
        // Wire AI to each strategy's actual params (not a disconnected copy).
        auto all_params = strategy_mgr.all_params();
        heartbeat = std::make_unique<pulse::heartbeat::HeartbeatScheduler>(
            cfg.ai, ai_pipeline, std::move(all_params));
        log->info("[L5] Heartbeat scheduler created (interval: {}s, {} strategy params)",
                  cfg.ai.heartbeatIntervalSec,
                  strategy_mgr.strategy_count());
    }
    else
    {
        log->info("[L4/L5] AI pipeline disabled (no API key or interval=0)");
    }

    // ------------------------------------------------------------------
    // 9. L9: WebUI (optional)
    // ------------------------------------------------------------------
#ifdef PULSE_ENABLE_WEBUI
    std::unique_ptr<pulse::webui::DashboardState> dashboard_state;
    std::unique_ptr<pulse::webui::WebServer>      web_server;

    if (cfg.webui.enabled)
    {
        // Pick whichever market feed/tracker is available.
        auto& ui_feed    = spot_feed    ? *spot_feed    : *futures_feed;
        auto& ui_tracker = spot_tracker ? *spot_tracker : *futures_tracker;

        dashboard_state = std::make_unique<pulse::webui::DashboardState>(
            cfg.webui, ui_feed, strategy_mgr, risk_mgr,
            ui_tracker, ai_pipeline);

        web_server = std::make_unique<pulse::webui::WebServer>(
            cfg.webui, *dashboard_state, "frontend");

        // Wire: dashboard snapshot → WS broadcast
        const auto& ws_ref = web_server->ws_server();
        dashboard_state->set_snapshot_callback(
            [&ws_ref](std::shared_ptr<const pulse::webui::DashboardSnapshot> snap)
            {
                // WsServer::push_snapshot is const-safe for broadcasting.
                const_cast<pulse::webui::WsServer&>(ws_ref).push_snapshot(snap);
            });

        log->info("[L9] WebUI: http://{}:{}", cfg.webui.bindAddress, cfg.webui.port);
    }
#else
    log->info("[L9] WebUI disabled (compile with -DPULSE_ENABLE_WEBUI=ON)");
#endif

    // ------------------------------------------------------------------
    // 10. Wire: aggregator output → risk check → execute → track
    // ------------------------------------------------------------------
    // Reservation tracking: maps order_id → reservation_id for TOCTOU-safe
    // notional reservation. Cancelled on order failure, consumed on fill.
    std::mutex reservation_mutex;
    std::unordered_map<std::string, std::uint64_t> order_reservations;

    aggregator.set_output_callback(
        [&](const pulse::strategy::TradingSignal& sig)
        {
            using namespace pulse;

            // Skip Flat signals.
            if (strategy::SignalType::Flat == sig.type)
            {
                return;
            }

            auto log_app = logging::Logger::get("app");

            // Build order request from signal.
            Side side = (strategy::SignalType::Buy == sig.type)
                        ? Side::Buy : Side::Sell;

            execution::OrderRequest req;
            req.symbol   = sig.symbol;
            req.side     = side;
            req.type     = OrderType::Market;
            req.price    = sig.price;
            req.market_type = sig.market_type;

            // Find strategy config for leverage/quantity settings.
            req.quantity = 0.001;
            for (const auto& inst : cfg.strategy.strategies)
            {
                if (inst.name == sig.strategy_id)
                {
                    req.quantity = inst.order_quantity;
                    req.market_type = inst.market_type;
                    req.leverage = inst.leverage;
                    break;
                }
            }

            // Risk evaluation.
            auto eval = risk_mgr.evaluate_order(req);
            if (risk::RiskDecision::Rejected == eval.decision)
            {
                log_app->warn("Signal REJECTED [{}] {} {} @ {:.2f} — {}",
                              sig.strategy_id,
                              sig.symbol,
                              (Side::Buy == side) ? "BUY" : "SELL",
                              sig.price,
                              eval.reason_message);
                return;
            }

            // Apply risk-modified quantity.
            if (risk::RiskDecision::Modified == eval.decision)
            {
                req.quantity = eval.approved_qty;
                log_app->info("Signal MODIFIED: qty reduced to {:.6f}",
                              eval.approved_qty);
            }

            log_app->info("Placing {} order: {} {:.6f} {} @ ~{:.2f} "
                          "(conf={:.2f}, reason={})",
                          (Side::Buy == side) ? "BUY" : "SELL",
                          sig.strategy_id,
                          req.quantity,
                          req.symbol,
                          sig.price,
                          sig.confidence,
                          sig.reason);

            // Place order via REST.
            auto* exec_ptr = (MarketType::Futures == req.market_type && futures_executor)
                ? futures_executor.get() : spot_executor.get();
            auto* track_ptr = (MarketType::Futures == req.market_type && futures_tracker)
                ? futures_tracker.get() : spot_tracker.get();

            auto result = exec_ptr->place_order(req);
            if (!ok(result))
            {
                auto err = error(result);
                log_app->error("Order FAILED: {} (code={})",
                               err.message, static_cast<int>(err.code));
                // Cancel the notional reservation since the order failed.
                if (eval.reservation_id > 0)
                {
                    position_mgr.cancel_reservation(eval.reservation_id);
                }
                return;
            }

            auto& resp = value(result);
            log_app->info("Order PLACED: id={} status={}",
                          resp.order_id,
                          static_cast<int>(resp.status));

            // Store reservation_id for the completion handler to consume.
            if (eval.reservation_id > 0)
            {
                std::lock_guard lock(reservation_mutex);
                order_reservations[resp.order_id] = eval.reservation_id;
            }

            // Track order lifecycle via WS + REST polling fallback.
            if (track_ptr)
            {
                track_ptr->track_order(resp.order_id, req.symbol,
                                        req.side, req.type,
                                        req.quantity, sig.price,
                                        sig.strategy_id);
            }
        });

    // ------------------------------------------------------------------
    // 11. Wire: order completion → update position manager + log
    // ------------------------------------------------------------------
    // Shared completion handler for both spot and futures trackers.
    auto completion_handler = [&](const pulse::execution::ExecutionReport& report)
    {
            auto log_app = pulse::logging::Logger::get("app");

            log_app->info("Order COMPLETED: id={} {} {} {:.6f} @ {:.2f} "
                          "fees={:.4f} slippage={:.2f}bps latency={}ms",
                          report.order_id,
                          report.symbol,
                          (pulse::Side::Buy == report.side) ? "BUY" : "SELL",
                          report.filled_qty,
                          report.avg_fill_price,
                          report.fees,
                          report.slippage_bps,
                          report.latency.count());

            // Update position manager and compute realized PnL.
            double pnl = 0.0;
            if (pulse::Side::Buy == report.side)
            {
                // Consume the notional reservation (if any) before opening.
                {
                    std::lock_guard lock(reservation_mutex);
                    auto it = order_reservations.find(report.order_id);
                    if (it != order_reservations.end())
                    {
                        position_mgr.consume_reservation(it->second);
                        order_reservations.erase(it);
                    }
                }

                auto open_result = position_mgr.open_position(
                    report.symbol,
                    report.side,
                    report.filled_qty,
                    report.avg_fill_price,
                    report.client_order_id);
                if (!pulse::ok(open_result))
                {
                    log_app->warn("Failed to open position: {}",
                                  pulse::error(open_result).message);
                }
            }
            else
            {
                // Consume the notional reservation for sell orders too.
                {
                    std::lock_guard lock(reservation_mutex);
                    auto it = order_reservations.find(report.order_id);
                    if (it != order_reservations.end())
                    {
                        position_mgr.consume_reservation(it->second);
                        order_reservations.erase(it);
                    }
                }

                // For sells, try to close matching positions.
                auto positions = position_mgr.get_positions_by_symbol(
                    report.symbol);
                for (const auto& pos : positions)
                {
                    auto close_result = position_mgr.close_position(
                            pos.position_id, report.filled_qty,
                            report.avg_fill_price);
                    if (close_result.has_value())
                    {
                        pnl += close_result.value();
                        log_app->info("Closed position {} (realized PnL: {:.4f})",
                                      pos.position_id, close_result.value());
                    }
                    else
                    {
                        log_app->warn("Failed to close position {}",
                                      pos.position_id);
                    }
                }
            }

            // Update drawdown guard with realized PnL.
            drawdown_guard.record_pnl(pnl);

            // Record trade in SQLite (if enabled).
#ifdef PULSE_ENABLE_SQLITE
            if (trade_recorder)
            {
                auto rec_result = trade_recorder->record_trade(
                    report, pnl, report.client_order_id);

                if (!pulse::ok(rec_result))
                {
                    log_app->warn("Trade recorder INSERT failed: {}",
                                  pulse::error(rec_result).message);
                }
            }
#endif
    };

    if (spot_tracker)
    {
        spot_tracker->set_completion_callback(completion_handler);
    }
    if (futures_tracker)
    {
        futures_tracker->set_completion_callback(completion_handler);
    }

    // ------------------------------------------------------------------
    // 12. Signal handler — SIGINT / SIGTERM
    // ------------------------------------------------------------------
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    // ------------------------------------------------------------------
    // 13. Start all layers
    // ------------------------------------------------------------------
    log->info("Starting trading engine...");

    // L1: WebSocket connections.
    if (spot_ws)
    {
        spot_ws->start();
        log->info("[L1] Spot WebSocket connecting...");
    }
    if (futures_ws)
    {
        futures_ws->start();
        log->info("[L1] Futures WebSocket connecting...");
    }

    // L3: Subscribe to market data channels.
    if (spot_feed)
    {
        spot_feed->start(cfg.symbols);
    }
    if (futures_feed)
    {
        futures_feed->start(cfg.symbols);
    }
    log->info("[L3] Market feed(s) started for {} symbol(s)", cfg.symbols.size());

    // L6: Spawn strategy threads.
    strategy_mgr.start();
    log->info("[L6] {} strategy thread(s) started", strategy_mgr.running_count());

    // L5: Start AI heartbeat.
    if (heartbeat)
    {
        heartbeat->start();
        log->info("[L5] Heartbeat scheduler started");
    }

    // L9: Start WebUI.
#ifdef PULSE_ENABLE_WEBUI
    if (dashboard_state)
    {
        dashboard_state->start();
    }
    if (web_server)
    {
        if (web_server->start())
        {
            log->info("[L9] WebUI server listening on port {}",
                      web_server->port());
        }
        else
        {
            log->warn("[L9] WebUI server failed to start");
        }
    }
#endif

    // ------------------------------------------------------------------
    // 14. Main loop — wait for stop signal with periodic heartbeat
    // ------------------------------------------------------------------
    log->info("Trading engine started. Press Ctrl+C to stop.");
    log->info("──────────────────────────────────────────────────");

    // Capture start time for uptime calculation.
    const auto engine_start = std::chrono::steady_clock::now();

    // Heartbeat interval: 60 seconds / 200ms sleep = 300 iterations.
    constexpr int kHeartbeatIntervalTicks = 300;
    int heartbeat_counter = 0;

    while (!g_stop_requested.load(std::memory_order_acquire))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        if (++heartbeat_counter >= kHeartbeatIntervalTicks)
        {
            heartbeat_counter = 0;
            log_system_heartbeat(
                engine_start,
                spot_feed.get(),
                futures_feed.get(),
                spot_ws.get(),
                futures_ws.get(),
                strategy_mgr,
                position_mgr);
        }
    }

    // ------------------------------------------------------------------
    // 15. Graceful shutdown (reverse order)
    // ------------------------------------------------------------------
    log->info("──────────────────────────────────────────────────");
    log->info("Shutdown signal received. Stopping trading engine...");

    // L9: WebUI
#ifdef PULSE_ENABLE_WEBUI
    if (web_server)
    {
        web_server->stop();
        log->info("[L9] WebUI server stopped");
    }
    if (dashboard_state)
    {
        dashboard_state->stop();
    }
#endif

    // L5: Heartbeat
    if (heartbeat)
    {
        heartbeat->stop();
        log->info("[L5] Heartbeat scheduler stopped ({} beats total)",
                  heartbeat->beat_count());
    }

    // L6: Strategy threads
    strategy_mgr.stop();
    log->info("[L6] Strategy threads stopped");

    // L3: Market feeds
    if (futures_feed) futures_feed->stop();
    if (spot_feed) spot_feed->stop();
    log->info("[L3] Market feed(s) stopped");

    // L1: WebSockets
    if (futures_ws) futures_ws->stop();
    if (spot_ws) spot_ws->stop();
    log->info("[L1] WebSocket(s) disconnected");

    // Summary
    auto portfolio = position_mgr.portfolio_summary();
    log->info("Final portfolio: {} open position(s), notional {:.2f} USDT",
              position_mgr.open_position_count(), portfolio.total_notional);

    // L8+: Trade recorder
#ifdef PULSE_ENABLE_SQLITE
    if (trade_recorder)
    {
        const auto count = trade_recorder->trade_count();
        trade_recorder->checkpoint();
        trade_recorder->close();
        log->info("[L8+] Trade recorder closed ({} trades recorded)", count);
    }
#endif

    log->info("pulseTrader stopped. Goodbye.");

    // L2: Logger (must be last)
    pulse::logging::Logger::shutdown();

    return 0;
}
