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
//   L9 Control   → JSON-RPC control socket (CLI/MCP)
//
// Usage:
//   pulsetrader                          Start with default config + .env credentials
//   pulsetrader --config trading.toml    Start with TOML config file
//   pulsetrader --help                   Print usage
//   pulsetrader --version                Print version

#include "core/SingleInstanceGuard.hpp"
#include "core/config.hpp"
#include "core/config_loader.hpp"
#include "core/config_validator.hpp"
#include "core/types.hpp"
#include "exchange/GateRestClient.hpp"
#include "market/SymbolRegistry.hpp"
#include "exchange/GateWsClient.hpp"
#include "logging/Logger.hpp"
#include "market/MarketFeed.hpp"
#include "risk/DrawdownGuard.hpp"
#include "risk/OrderRateLimiter.hpp"
#include "risk/PositionManager.hpp"
#include "risk/RiskManager.hpp"
#include "execution/OrderExecutor.hpp"
#include "execution/OrderTracker.hpp"
#include "strategy/StrategyManager.hpp"
#include "strategy/signal/SignalAggregator.hpp"
#include "strategy/scalping/MomentumScalper.hpp"
#include "strategy/scalping/OrderBookScalper.hpp"
#include "strategy/scalping/MeanReversionScalper.hpp"
#include "strategy/scalping/SuperTrendScalper.hpp"
#include "ai/AiPipeline.hpp"
#include "heartbeat/HeartbeatScheduler.hpp"
#include "control/CommandParser.hpp"
#include "control/ControlClient.hpp"
#include "control/EngineServices.hpp"
#include "control/JsonRpcServer.hpp"
#include "control/McpServer.hpp"
#include "control/OrderFlowExecutor.hpp"

#include <fmt/ranges.h>


#ifdef PULSE_ENABLE_SQLITE
#include "trade_recorder/TradeRecorder.hpp"
#endif

#include <atomic>
#include <cerrno>
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

#include <poll.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Global stop flag — set by SIGINT / SIGTERM handler
// ---------------------------------------------------------------------------
static std::atomic<bool> g_stop_requested{ false };

static void signalHandler(int /*sig*/)
{
    g_stop_requested.store(true, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static std::string envOr(const char* name, const std::string& fallback)
{
    const char* val = std::getenv(name);
    return (val && val[0]) ? std::string(val) : fallback;
}

static void printUsage(const char* prog)
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
        << "  PULSE_CONTROL_PORT Control socket port (default: 8081)\n\n"
        << "TOML config (--config):\n"
        << "  Use from_env:VAR_NAME syntax to read sensitive values from env.\n"
        << "  Example: apiKey = \"from_env:GATE_API_KEY\"\n"
        << "  See trading.toml.example for a complete template.\n"
        << std::endl;
}

// ---------------------------------------------------------------------------
// Build default PulseConfig with env-var overrides
// ---------------------------------------------------------------------------
static pulse::PulseConfig buildDefaultConfig()
{
    using namespace pulse;

    PulseConfig cfg;

    // Detect network mode: "mainnet" (default) or "testnet".
    std::string network = envOr("PULSE_NETWORK", "mainnet");
    bool is_testnet = ("testnet" == network);

    // L1: Exchange — credentials and URLs depend on network mode.
    if (is_testnet)
    {
        cfg.exchange.testnet = true;
        cfg.exchange.apiKey    = envOr("GATE_TESTNET_API_KEY", "");
        cfg.exchange.apiSecret = envOr("GATE_TESTNET_API_SECRET", "");
        cfg.exchange.restBaseUrl   = pulse::url::kTestnetRest;
        cfg.exchange.wsUrl         = pulse::url::kTestnetSpotWs;
        cfg.exchange.futuresWsUrl  = pulse::url::kTestnetFuturesWs;
    }
    else
    {
        // Backward compatible: try GATE_MAINNET_* first, fall back to GATE_*.
        cfg.exchange.apiKey = envOr("GATE_MAINNET_API_KEY",
                                     envOr("GATE_API_KEY", ""));
        cfg.exchange.apiSecret = envOr("GATE_MAINNET_API_SECRET",
                                        envOr("GATE_API_SECRET", ""));
        cfg.exchange.restBaseUrl = envOr("GATE_MAINNET_REST_URL",
                                          "https://api.gateio.ws");
        cfg.exchange.wsUrl = envOr("GATE_MAINNET_SPOT_WS_URL",
                                    "wss://api.gateio.ws/ws/v4/");
        cfg.exchange.futuresWsUrl = envOr("GATE_MAINNET_FUTURES_WS_URL",
                                           "wss://fx-ws.gateio.ws/v4/ws/usdt");
    }

    cfg.exchange.proxyUrl   = envOr("HTTPS_PROXY", envOr("HTTP_PROXY", ""));

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
    cfg.ai.apiKey               = envOr("OPENAI_API_KEY", "");
    cfg.ai.heartbeatIntervalSec = 0;  // Disabled until AI key is provided.
    cfg.ai.requestTimeoutMs     = 30'000;

    // Control plane: JSON-RPC control socket.
    cfg.control.enabled     = true;
    cfg.control.bindAddress = "127.0.0.1";
    cfg.control.port        = static_cast<std::uint16_t>(
        std::stoi(envOr("PULSE_CONTROL_PORT", "8081")));

    return cfg;
}

// ---------------------------------------------------------------------------
// Create a strategy instance from config
// ---------------------------------------------------------------------------
static std::unique_ptr<pulse::strategy::StrategyBase>
createStrategy(const std::string& name,
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
    if ("supertrend_scalper" == name)
    {
        return std::make_unique<SuperTrendScalper>(ctx);
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// logSystemHeartbeat — periodic system health summary
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
static void logSystemHeartbeat(
    std::chrono::steady_clock::time_point start_time,
    const pulse::market::MarketFeed* spot_feed,
    const pulse::market::MarketFeed* futures_feed,
    const pulse::market::MarketFeed* cfd_feed,
    const pulse::exchange::GateWsClient* spot_ws,
    const pulse::exchange::GateWsClient* futures_ws,
    const pulse::strategy::StrategyManager& strategy_mgr,
    const pulse::risk::PositionManager& position_mgr,
    pulse::exchange::GateRestClient* rest_client,
    pulse::exchange::GateRestClient* spot_rest_client,
    pulse::exchange::GateRestClient* cfd_rest_client,
    std::mutex& rest_mutex)
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
    static pulse::market::FeedStats prev_cfd     = { 0, 0, 0 };
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

    // CFD section (REST poll feed; orderbook count is always 0).
    if (nullptr != cfd_feed)
    {
        msg << " | cfd ";
        format_feed(cfd_feed, prev_cfd, msg);
    }

    // WS status.
    msg << " | ws spot=" << ws_label(spot_ws)
        << " futures=" << ws_label(futures_ws);

    // Strategy status.
    msg << " | strategies " << strategy_mgr.runningCount()
        << "/" << strategy_mgr.strategyCount() << " running";

    // Position status.
    const auto portfolio = position_mgr.portfolioSummary();
    msg << " | positions " << position_mgr.openPositionCount()
        << " (notional " << std::fixed << std::setprecision(2)
        << portfolio.total_notional << " USDT)";

    // Account balance (fetched via REST, cached).
    // REST clients are not thread-safe — serialize with the control plane.
    std::lock_guard rest_lock(rest_mutex);
    if (nullptr != rest_client)
    {
        auto bal_result = rest_client->getFuturesAccountBalance();
        if (ok(bal_result))
        {
            const auto &bal = value(bal_result);
            msg << " | futures " << std::fixed << std::setprecision(2)
                << bal.total << " " << bal.currency
                << " (avail " << bal.available
                << ", pnl " << (bal.unrealised_pnl >= 0 ? "+" : "")
                << bal.unrealised_pnl << ")";
        }
    }

    // Spot account balance.
    if (nullptr != spot_rest_client)
    {
        auto spot_result = spot_rest_client->getSpotAccounts();
        if (ok(spot_result))
        {
            const auto &arr = value(spot_result);
            for (const auto &item : arr)
            {
                if ("USDT" == item.value("currency", ""))
                {
                    double avail = pulse::safeParseDouble(item.value("available", "0")).value_or(0.0);
                    double locked = pulse::safeParseDouble(item.value("locked", "0")).value_or(0.0);
                    msg << " | spot " << std::fixed << std::setprecision(2)
                        << (avail + locked) << " USDT"
                        << " (avail " << avail << ")";
                    break;
                }
            }
        }
    }

    // CFD account balance (USD, MT5 account).
    if (nullptr != cfd_rest_client)
    {
        auto cfd_result = cfd_rest_client->getCfdAssets();
        if (ok(cfd_result))
        {
            const auto &data = value(cfd_result).value("data", nlohmann::json::object());
            const double equity = pulse::safeParseDouble(data.value("equity", "0")).value_or(0.0);
            const double free   = pulse::safeParseDouble(data.value("margin_free", "0")).value_or(0.0);
            msg << " | cfd " << std::fixed << std::setprecision(2)
                << equity << " USD (avail " << free << ")";
        }
    }

    PULSE_LOG_INFO("system", "{}", msg.str());

    first_call = false;
}

// ===========================================================================
// main
// ===========================================================================

// Forward declaration — defined after runCli; called from runTrade at startup.
static void syncFuturesPositionsFromExchange(
    pulse::exchange::GateRestClient &futures_rest,
    pulse::risk::PositionManager &position_mgr,
    const std::shared_ptr<pulse::market::SymbolRegistry> &registry,
    std::mutex &rest_mutex);

static int runTrade(int argc, char* argv[])
{
    using pulse::MarketType;
    using pulse::MarginMode;

    // ------------------------------------------------------------------
    // 0. Single-instance guard — refuse a second engine on this machine.
    //    Two engines ran simultaneously on 2026-08-16 and each only knew
    //    its own fills, so the exchange position drifted from the engine
    //    view. The flock lives for the whole process (RAII).
    // ------------------------------------------------------------------
    std::optional<pulse::SingleInstanceGuard> instance_guard;
    if (nullptr == std::getenv("PULSE_ALLOW_MULTI_INSTANCES"))
    {
        instance_guard.emplace("data/engine.lock");
        if (!instance_guard->acquired())
        {
            std::cerr << "[fatal] Another pulseTrader engine instance is "
                         "already running (lock: data/engine.lock).\n"
                         "Stopping this instance to avoid double trading.\n"
                         "  - check `systemctl --user status pulsetrader`\n"
                         "  - or set PULSE_ALLOW_MULTI_INSTANCES=1 to "
                         "bypass (not recommended)\n";
            return 1;
        }
    }

    // ------------------------------------------------------------------
    // 1. Command-line parsing
    // ------------------------------------------------------------------
    std::string config_path;

    for (int i = 0; i < argc; ++i)
    {
        std::string arg(argv[i]);

        if ("--help" == arg || "-h" == arg)
        {
            printUsage(argv[0]);
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
        printUsage(argv[0]);
        return 1;
    }

    // ------------------------------------------------------------------
    // 1. Build configuration
    // ------------------------------------------------------------------
    pulse::PulseConfig cfg;

    if (!config_path.empty())
    {
        // Load from TOML file.
        auto result = pulse::loadConfigFile(config_path);

        if (!pulse::ok(result))
        {
            std::cerr << "Config error: "
                      << pulse::error(result).message << "\n";
            return 1;
        }

        cfg = pulse::value(result);

        // Note: testnet URL switching is handled by config_loader::parseExchange()
        // which reads testnet flag first and sets URL defaults accordingly.
    }
    else
    {
        cfg = buildDefaultConfig();
    }

    // Validate regardless of source.
    auto validation_err = pulse::validateConfig(cfg);

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
    // 3. L1: Exchange clients (tri-market support: spot / futures / CFD)
    // ------------------------------------------------------------------
    // Shared REST serialization mutex (GateRestClient is not thread-safe;
    // heartbeat balance queries, control-plane REST calls and the CFD poll
    // thread all share it). Declared early — the CFD feed needs it.
    std::mutex rest_mutex;

    // Detect which market types are needed by enabled strategies.
    bool has_spot = false;
    bool has_futures = false;
    bool has_cfd = false;
    for (const auto& inst : cfg.strategy.strategies)
    {
        if (!inst.enabled) { continue; }
        switch (inst.market_type)
        {
        case MarketType::Futures: has_futures = true; break;
        case MarketType::Cfd:     has_cfd = true;     break;
        default:                  has_spot = true;    break;
        }
    }
    // Default to spot if no strategies configured (backward compatibility).
    if (!has_spot && !has_futures && !has_cfd) has_spot = true;

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

    // TradFi CFD infrastructure (REST-only — no WebSocket client at all).
    std::unique_ptr<pulse::exchange::GateRestClient> cfd_rest;
    std::unique_ptr<pulse::market::MarketFeed>       cfd_feed;
    std::unique_ptr<pulse::execution::OrderExecutor>  cfd_executor;
    std::unique_ptr<pulse::execution::OrderTracker>   cfd_tracker;

    if (has_spot)
    {
        spot_rest = std::make_unique<pulse::exchange::GateRestClient>(
            cfg.exchange, MarketType::Spot);
        spot_ws = std::make_unique<pulse::exchange::GateWsClient>(
            cfg.exchange, MarketType::Spot);
        spot_feed = std::make_unique<pulse::market::MarketFeed>(
            spot_ws.get(), *spot_rest, MarketType::Spot);
        spot_executor = std::make_unique<pulse::execution::OrderExecutor>(
            *spot_rest, MarketType::Spot);
        spot_tracker = std::make_unique<pulse::execution::OrderTracker>(
            spot_ws.get(), *spot_rest, MarketType::Spot);
        log->info("[L1] Spot exchange clients created");
    }

    // Spot REST client for balance queries (always created when credentials
    // exist, even without spot strategies — needed for CLI/MCP balance queries).
    if (!has_spot)
    {
        spot_rest = std::make_unique<pulse::exchange::GateRestClient>(
            cfg.exchange, MarketType::Spot);
        log->info("[L1] Spot REST client created (balance queries only)");
    }

    if (has_futures)
    {
        futures_rest = std::make_unique<pulse::exchange::GateRestClient>(
            cfg.exchange, MarketType::Futures);
        futures_ws = std::make_unique<pulse::exchange::GateWsClient>(
            cfg.exchange, MarketType::Futures);
        futures_feed = std::make_unique<pulse::market::MarketFeed>(
            futures_ws.get(), *futures_rest, MarketType::Futures);
        futures_executor = std::make_unique<pulse::execution::OrderExecutor>(
            *futures_rest, MarketType::Futures);
        futures_tracker = std::make_unique<pulse::execution::OrderTracker>(
            futures_ws.get(), *futures_rest, MarketType::Futures);
        log->info("[L1] Futures exchange clients created");
    }

    if (has_cfd)
    {
        cfd_rest = std::make_unique<pulse::exchange::GateRestClient>(
            cfg.exchange, MarketType::Cfd);
        // REST-poll feed: no WS client, shared rest_mutex for poll requests.
        cfd_feed = std::make_unique<pulse::market::MarketFeed>(
            nullptr, *cfd_rest, MarketType::Cfd, &rest_mutex);
        cfd_executor = std::make_unique<pulse::execution::OrderExecutor>(
            *cfd_rest, MarketType::Cfd);
        cfd_tracker = std::make_unique<pulse::execution::OrderTracker>(
            nullptr, *cfd_rest, MarketType::Cfd, /*enable_ws=*/false);
        log->info("[L1] TradFi CFD exchange clients created (REST poll mode)");
    }

    // ------------------------------------------------------------------
    // 4. L3: Market Data (per-market feeds already created above)
    // ------------------------------------------------------------------
    log->info("[L3] Market feed(s) ready (spot={}, futures={}, cfd={})",
              has_spot ? "yes" : "no",
              has_futures ? "yes" : "no",
              has_cfd ? "yes" : "no");

    // ------------------------------------------------------------------
    // 5. L7: Risk Management
    // ------------------------------------------------------------------
    pulse::risk::PositionManager   position_mgr(cfg.risk);
    pulse::risk::DrawdownGuard     drawdownGuard(cfg.risk);
    pulse::risk::OrderRateLimiter  rateLimiter(cfg.risk.maxOrdersPerSec);
    pulse::risk::RiskManager       risk_mgr(cfg.risk, position_mgr,
                                            drawdownGuard, rateLimiter);

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

        switch (inst_cfg.market_type)
        {
        case pulse::MarketType::Futures:
            feed_ptr = futures_feed.get();
            exec_ptr = futures_executor.get();
            break;
        case pulse::MarketType::Cfd:
            feed_ptr = cfd_feed.get();
            exec_ptr = cfd_executor.get();
            break;
        default:
            feed_ptr = spot_feed.get();
            exec_ptr = spot_executor.get();
            break;
        }

        if (!feed_ptr || !exec_ptr)
        {
            log->warn("No {} infrastructure available for strategy '{}', skipping",
                      pulse::toString(inst_cfg.market_type),
                      inst_cfg.name);
            continue;
        }

        pulse::strategy::StrategyContext ctx(*feed_ptr, risk_mgr,
                                             *exec_ptr, inst_cfg);

        auto strat = createStrategy(inst_cfg.name, ctx);
        if (!strat)
        {
            log->warn("Unknown strategy '{}', skipping", inst_cfg.name);
            continue;
        }

        log->info("[L6] Registered strategy: {} on {} (qty={}, conf={:.2f}, market={})",
                  inst_cfg.name, inst_cfg.symbol,
                  inst_cfg.order_quantity, inst_cfg.min_confidence,
                  pulse::toString(inst_cfg.market_type));

        strategy_mgr.registerStrategy(std::move(strat));
    }

    if (0 == strategy_mgr.strategyCount())
    {
        log->error("No strategies registered. Exiting.");
        pulse::logging::Logger::shutdown();
        return 1;
    }

    // Wire: strategy signals → aggregator
    strategy_mgr.setSignalCallback(
        [&aggregator](const pulse::strategy::TradingSignal& sig)
        {
            aggregator.addSignal(sig);
        });

    log->info("[L6] Strategy engine ready ({} instances, threshold={:.2f}, "
              "cooldown={}s)",
              strategy_mgr.strategyCount(),
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
        auto allParams = strategy_mgr.allParams();
        heartbeat = std::make_unique<pulse::heartbeat::HeartbeatScheduler>(
            cfg.ai, ai_pipeline, std::move(allParams));
        log->info("[L5] Heartbeat scheduler created (interval: {}s, {} strategy params)",
                  cfg.ai.heartbeatIntervalSec,
                  strategy_mgr.strategyCount());
    }
    else
    {
        log->info("[L4/L5] AI pipeline disabled (no API key or interval=0)");
    }

    // ------------------------------------------------------------------
    // 10. Wire: aggregator output → risk check → execute → track
    // ------------------------------------------------------------------
    // Order flow executor: owns the reservation map and the full
    // risk-gated execution flow (aggregator + manual CLI/MCP paths).
    // Placers are constructed only for markets that have an executor.
    std::unique_ptr<pulse::control::ExecutorOrderPlacer> spot_placer_impl;
    std::unique_ptr<pulse::control::ExecutorOrderPlacer> futures_placer_impl;
    std::unique_ptr<pulse::control::ExecutorOrderPlacer> cfd_placer_impl;
    if (spot_executor)
    {
        spot_placer_impl = std::make_unique<pulse::control::ExecutorOrderPlacer>(
            *spot_executor);
    }
    if (futures_executor)
    {
        futures_placer_impl = std::make_unique<pulse::control::ExecutorOrderPlacer>(
            *futures_executor);
    }
    if (cfd_executor)
    {
        cfd_placer_impl = std::make_unique<pulse::control::ExecutorOrderPlacer>(
            *cfd_executor);
    }
    pulse::control::OrderFlowExecutor order_flow(
        cfg.strategy,
        risk_mgr,
        position_mgr,
        drawdownGuard,
        spot_placer_impl ? spot_placer_impl.get() : nullptr,
        futures_placer_impl ? futures_placer_impl.get() : nullptr,
        cfd_placer_impl ? cfd_placer_impl.get() : nullptr,
        spot_tracker.get(),
        futures_tracker.get(),
        cfd_tracker.get(),
        rest_mutex
#ifdef PULSE_ENABLE_SQLITE
        , trade_recorder.get()
#endif
    );

    // Instrument metadata (contract multipliers) — fetched once at startup so
    // the risk gate computes futures notional correctly: qty is in contracts,
    // so notional = qty * price * quanto_multiplier (1 BTC_USDT contract =
    // 0.0001 BTC). Without it, a 1-contract order would be treated as 1 BTC.
    // The CFD registry (contract_volume per symbol) merges into the same
    // shared lookup: XAUUSD's 100 oz/lot plays the same multiplier role.
    std::shared_ptr<pulse::market::SymbolRegistry> symbol_registry;
    if (futures_rest)
    {
        symbol_registry = std::make_shared<pulse::market::SymbolRegistry>(
            *futures_rest, pulse::MarketType::Futures);
        if (symbol_registry->loadFromRest())
        {
            log->info("[L1] Symbol metadata loaded ({} futures contracts)",
                      symbol_registry->size());
        }
        else
        {
            log->warn("[L1] Symbol metadata load failed — futures notional "
                      "falls back to qty x price (contracts treated as units)");
        }
    }
    if (cfd_rest)
    {
        pulse::market::SymbolRegistry cfd_registry(*cfd_rest,
                                                   pulse::MarketType::Cfd);
        if (cfd_registry.loadFromRest(cfg.symbols))
        {
            log->info("[L1] CFD symbol metadata loaded ({} symbols)",
                      cfd_registry.size());
            if (!symbol_registry)
            {
                symbol_registry = std::make_shared<pulse::market::SymbolRegistry>(
                    futures_rest ? *futures_rest : *cfd_rest,
                    pulse::MarketType::Futures);
            }
            symbol_registry->mergeFrom(cfd_registry);
        }
        else
        {
            log->warn("[L1] CFD symbol metadata load failed — CFD notional "
                      "falls back to qty x price (lots treated as units)");
        }
    }
    if (symbol_registry)
    {
        order_flow.setSymbolRegistry(symbol_registry);
    }

    // Reconcile positions that already exist on the exchange (previous
    // engine run, manual trading) so risk limits and displays reflect the
    // true exposure from the first second. Non-fatal on failure.
    if (futures_rest)
    {
        syncFuturesPositionsFromExchange(
            *futures_rest, position_mgr, symbol_registry, rest_mutex);
    }

    aggregator.setOutputCallback(
        [&order_flow](const pulse::strategy::TradingSignal& sig)
        {
            order_flow.onSignal(sig);
        });

    // Completion callbacks are wired by OrderFlowExecutor's constructor
    // (consume reservation → open/close position → drawdown PnL → SQLite).


    // ------------------------------------------------------------------
    // 11b. Control plane: EngineServices + JSON-RPC control socket
    // ------------------------------------------------------------------
    const auto engine_start_ref = std::chrono::steady_clock::now();
    pulse::control::EngineServices services(
        "0.1.0",
        engine_start_ref,
        cfg,
        strategy_mgr,
        risk_mgr,
        position_mgr,
        spot_feed.get(),
        futures_feed.get(),
        cfd_feed.get(),
        spot_rest.get(),
        futures_rest.get(),
        cfd_rest.get(),
        spot_tracker.get(),
        futures_tracker.get(),
        cfd_tracker.get(),
        order_flow,
        rest_mutex);

    // The registry is copied into the server; the local copy is used by
    // the embedded REPL (both dispatch to the same EngineServices).
    auto control_registry = pulse::control::makeMethodRegistry(services);
    std::unique_ptr<pulse::control::JsonRpcServer> control_server;
    if (cfg.control.enabled)
    {
        control_server = std::make_unique<pulse::control::JsonRpcServer>(
            cfg.control.bindAddress, cfg.control.port, control_registry);
    }

    // ------------------------------------------------------------------
    // 12. Signal handler — SIGINT / SIGTERM
    // ------------------------------------------------------------------
    std::signal(SIGINT,  signalHandler);
    std::signal(SIGTERM, signalHandler);

    // ------------------------------------------------------------------
    // 12b. Direction gate — activate the configured default direction.
    // ------------------------------------------------------------------
    // The active direction comes from `active_market` (default "futures" —
    // "暂时只做合约方向"). Every other direction's strategies pause so they
    // cannot trade until switch_direction activates them; the order-flow
    // gate in OrderFlowExecutor is the second, independent guarantee.
    order_flow.setActiveMarket(cfg.active_market);
    const int resumed_at_start = strategy_mgr.setPausedByMarket(cfg.active_market, false);
    int paused_at_start = 0;
    for (const auto& inst_cfg : cfg.strategy.strategies)
    {
        if (inst_cfg.enabled && inst_cfg.market_type != cfg.active_market)
        {
            paused_at_start += strategy_mgr.setPausedByMarket(inst_cfg.market_type, true);
        }
    }
    log->info("[L6] Active trading direction: {} ({} resumed, {} paused until switched)",
              pulse::toString(cfg.active_market), resumed_at_start, paused_at_start);

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
    // The CFD feed's REST poll thread starts regardless of the active
    // direction — switching to CFD must be instant (no warm-up wait).
    if (spot_feed)
    {
        spot_feed->start(cfg.symbols);
    }
    if (futures_feed)
    {
        futures_feed->start(cfg.symbols);
    }
    if (cfd_feed)
    {
        cfd_feed->start(cfg.symbols);
    }
    log->info("[L3] Market feed(s) started for {} symbol(s)", cfg.symbols.size());

    // L6: Spawn strategy threads.
    strategy_mgr.start();
    log->info("[L6] {} strategy thread(s) started", strategy_mgr.runningCount());

    // L5: Start AI heartbeat.
    if (heartbeat)
    {
        heartbeat->start();
        log->info("[L5] Heartbeat scheduler started");
    }

    // Control plane: start JSON-RPC control socket.
    if (control_server)
    {
        if (!control_server->start())
        {
            log->warn("[L9] Control socket failed to bind {}:{} — "
                      "continuing without remote control",
                      cfg.control.bindAddress, cfg.control.port);
        }
    }

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

    // Embedded REPL: enabled when stdin is a TTY. Commands dispatch
    // directly to EngineServices in-process (no socket round-trip).
    bool repl_enabled = isatty(STDIN_FILENO);
    std::string repl_buffer;
    if (repl_enabled)
    {
        std::cout << "pulseTrader interactive mode — type 'help' for commands.\n";
        std::cout << "pulse> " << std::flush;
    }

    while (!g_stop_requested.load(std::memory_order_acquire))
    {
        if (repl_enabled)
        {
            // Poll stdin with a 200ms timeout (EINTR-safe).
            struct pollfd pfd{ STDIN_FILENO, POLLIN, 0 };
            int rc = ::poll(&pfd, 1, 200);
            if (rc > 0 && (pfd.revents & POLLIN))
            {
                char buf[512];
                const ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
                if (n > 0)
                {
                    repl_buffer.append(buf, static_cast<std::size_t>(n));
                    std::size_t newline;
                    while ((newline = repl_buffer.find('\n'))
                           != std::string::npos)
                    {
                        std::string line = repl_buffer.substr(0, newline);
                        repl_buffer.erase(0, newline + 1);
                        while (!line.empty() && '\r' == line.back())
                        {
                            line.pop_back();
                        }

                        if (!line.empty())
                        {
                            if ("quit" == line || "exit" == line)
                            {
                                // REPL exits; engine keeps running.
                                std::cout << "(REPL exited — engine continues. "
                                             "Ctrl+C to stop the engine)\n";
                            }
                            else if ("help" == line || "?" == line)
                            {
                                std::cout << pulse::control::replHelp();
                            }
                            else if (auto cmd = pulse::control::parseCommandLine(line))
                            {
                                const auto it = control_registry.find(cmd->method);
                                if (control_registry.end() == it)
                                {
                                    std::cout << "Unknown command. "
                                                 "Type 'help'.\n";
                                }
                                else
                                {
                                    const auto result = it->second(cmd->params);
                                    if (pulse::ok(result))
                                    {
                                        std::cout << pulse::control::formatResponse(
                                            cmd->method, pulse::value(result));
                                    }
                                    else
                                    {
                                        std::cout << "Error: "
                                                  << pulse::error(result).message
                                                  << "\n";
                                    }
                                }
                            }
                            else
                            {
                                std::cout << "Unknown command. Type 'help'.\n";
                            }
                        }
                        std::cout << "pulse> " << std::flush;
                    }
                }
                else if (0 == n)
                {
                    // EOF (Ctrl+D) — exit the REPL only.
                    std::cout << "\n(EOF — REPL exited, engine continues. "
                                 "Ctrl+C to stop)\n";
                    repl_enabled = false;
                }
            }
            else if (rc < 0 && EINTR != errno)
            {
                // poll error — fall back to sleeping.
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        }
        else
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        if (++heartbeat_counter >= kHeartbeatIntervalTicks)
        {
            heartbeat_counter = 0;
            logSystemHeartbeat(
                engine_start,
                spot_feed.get(),
                futures_feed.get(),
                cfd_feed.get(),
                spot_ws.get(),
                futures_ws.get(),
                strategy_mgr,
                position_mgr,
                futures_rest ? futures_rest.get() : spot_rest.get(),
                spot_rest ? spot_rest.get() : nullptr,
                cfd_rest ? cfd_rest.get() : nullptr,
                rest_mutex);
        }
    }

    // ------------------------------------------------------------------
    // 15. Graceful shutdown (reverse order)
    // ------------------------------------------------------------------
    log->info("──────────────────────────────────────────────────");
    log->info("Shutdown signal received. Stopping trading engine...");

    // L9: Control plane (stop before layers so remote clients drop cleanly)
    if (control_server)
    {
        control_server->stop();
        log->info("[L9] Control socket stopped");
    }

    // L5: Heartbeat
    if (heartbeat)
    {
        heartbeat->stop();
        log->info("[L5] Heartbeat scheduler stopped ({} beats total)",
                  heartbeat->beatCount());
    }

    // L6: Strategy threads
    strategy_mgr.stop();
    log->info("[L6] Strategy threads stopped");

    // L3: Market feeds (cfd feed joins its REST poll thread)
    if (futures_feed) { futures_feed->stop(); }
    if (spot_feed) { spot_feed->stop(); }
    if (cfd_feed) { cfd_feed->stop(); }
    log->info("[L3] Market feed(s) stopped");

    // L1: WebSockets
    if (futures_ws) { futures_ws->stop(); }
    if (spot_ws) { spot_ws->stop(); }
    log->info("[L1] WebSocket(s) disconnected");

    // Summary
    auto portfolio = position_mgr.portfolioSummary();
    log->info("Final portfolio: {} open position(s), notional {:.2f} USDT",
              position_mgr.openPositionCount(), portfolio.total_notional);

    // L8+: Trade recorder
#ifdef PULSE_ENABLE_SQLITE
    if (trade_recorder)
    {
        const auto count = trade_recorder->tradeCount();
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

// ===========================================================================
// Subcommand dispatch — trade (default) | cli | mcp
// ===========================================================================

namespace
{

/// Shared arg parsing for cli/mcp: --config, --host, --port, --help.
struct ClientArgs
{
    std::string config_path;
    std::string host;
    std::string port_str;
};

bool parseClientArgs(int argc, char *argv[], ClientArgs &out)
{
    for (int i = 0; i < argc; ++i)
    {
        std::string arg(argv[i]);
        if ("--help" == arg || "-h" == arg)
        {
            return false;
        }
        if ("--config" == arg && i + 1 < argc)
        {
            out.config_path = argv[++i];
        }
        else if ("--host" == arg && i + 1 < argc)
        {
            out.host = argv[++i];
        }
        else if ("--port" == arg && i + 1 < argc)
        {
            out.port_str = argv[++i];
        }
        else
        {
            std::cerr << "Unknown argument: " << arg << "\n";
            return false;
        }
    }
    return true;
}

/// Load control endpoint from config (with optional CLI overrides).
pulse::PulseConfig loadControlConfig(const ClientArgs &args)
{
    pulse::PulseConfig cfg;
    if (!args.config_path.empty())
    {
        auto result = pulse::loadConfigFile(args.config_path);
        if (pulse::ok(result))
        {
            cfg = pulse::value(result);
        }
        else
        {
            std::cerr << "Config error: " << pulse::error(result).message << "\n";
        }
    }
    else
    {
        cfg = buildDefaultConfig();
    }
    if (!args.host.empty())
    {
        cfg.control.bindAddress = args.host;
    }
    if (!args.port_str.empty())
    {
        cfg.control.port = static_cast<std::uint16_t>(
            std::stoi(args.port_str));
    }
    return cfg;
}

/// Interactive REPL loop over a ControlClient (remote attach).
int runCliRepl(pulse::control::ControlClient &client)
{
    std::cout << "Connected to pulseTrader engine at "
              << "control socket (type 'help' for commands, 'quit' to exit)\n";
    std::cout << "pulse> " << std::flush;

    std::string line;
    while (std::getline(std::cin, line))
    {
        if (line.empty())
        {
            std::cout << "pulse> " << std::flush;
            continue;
        }
        if ("quit" == line || "exit" == line)
        {
            break;
        }
        if ("help" == line || "?" == line)
        {
            std::cout << pulse::control::replHelp();
            std::cout << "pulse> " << std::flush;
            continue;
        }

        auto cmd = pulse::control::parseCommandLine(line);
        if (!cmd.has_value())
        {
            std::cout << "Unknown command. Type 'help'.\n";
            std::cout << "pulse> " << std::flush;
            continue;
        }

        auto resp = client.call(cmd->method, cmd->params);
        if (pulse::ok(resp))
        {
            std::cout << pulse::control::formatResponse(cmd->method,
                                                        pulse::value(resp));
        }
        else
        {
            std::cerr << "Error: " << pulse::error(resp).message << "\n";
        }
        std::cout << "pulse> " << std::flush;
    }
    return 0;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Position reconciliation — import positions that already exist on the
// exchange (opened by a previous engine run or manually) into the risk
// engine at startup. Without this, a restart silently forgets open
// positions: the engine view drifts from the exchange (2026-08-16 incident)
// and risk limits / displays undercount real exposure. Non-fatal on failure.
// ---------------------------------------------------------------------------

/// Read a numeric field that may arrive as a JSON string or number.
static double jsonNumber(const nlohmann::json &j, const char *key)
{
    const auto it = j.find(key);
    if (j.end() == it)
    {
        return 0.0;
    }
    if (it->is_string())
    {
        return pulse::safeParseDouble(it->get<std::string>()).value_or(0.0);
    }
    if (it->is_number())
    {
        return it->get<double>();
    }
    return 0.0;
}

static void syncFuturesPositionsFromExchange(
    pulse::exchange::GateRestClient &futures_rest,
    pulse::risk::PositionManager &position_mgr,
    const std::shared_ptr<pulse::market::SymbolRegistry> &registry,
    std::mutex &rest_mutex)
{
    nlohmann::json positions;
    {
        std::lock_guard<std::mutex> rest_lock(rest_mutex);
        auto result = futures_rest.getFuturesPositions();
        if (!pulse::ok(result))
        {
            PULSE_LOG_WARN("app", "Position sync skipped: {}",
                           pulse::error(result).message);
            return;
        }
        positions = pulse::value(result);
    }

    int synced = 0;
    for (const auto &p : positions)
    {
        const std::string contract = p.value("contract", "");
        if (contract.empty())
        {
            continue;
        }
        const int size = p.value("size", 0);
        if (0 == size)
        {
            continue;
        }

        const double entry = jsonNumber(p, "entry_price");
        const double mark = jsonNumber(p, "mark_price");

        // Gate reports leverage = 0 (as a STRING) for cross margin; fall
        // back to the account's cross leverage limit for margin/PnL math.
        double leverage = jsonNumber(p, "leverage");
        if (leverage <= 0.0)
        {
            leverage = jsonNumber(p, "cross_leverage_limit");
        }
        if (leverage <= 0.0)
        {
            leverage = 10.0;
        }

        double quanto_multiplier = 1.0;
        if (registry)
        {
            if (const auto info = registry->get(contract))
            {
                quanto_multiplier = info->quanto_multiplier;
            }
        }

        // Exchange open_time is a unix SECONDS string; Position stores ns.
        const double open_secs = jsonNumber(p, "open_time");
        pulse::Timestamp open_time{};
        if (open_secs > 0.0)
        {
            open_time = std::chrono::time_point_cast<pulse::Duration>(
                std::chrono::system_clock::time_point{
                    std::chrono::seconds{static_cast<std::int64_t>(open_secs)}});
        }

        const bool is_long = size > 0;
        position_mgr.syncPositionFromExchange(
            contract, is_long ? pulse::Side::Buy : pulse::Side::Sell,
            static_cast<double>(std::abs(size)), entry, mark,
            pulse::MarketType::Futures, leverage, pulse::MarginMode::Cross,
            quanto_multiplier, jsonNumber(p, "maintenance_rate"),
            jsonNumber(p, "liq_price"), open_time);

        PULSE_LOG_INFO("app",
            "Position sync: {} {} {} contracts @ {} (mark {}, liq {})",
            contract, (is_long ? "long" : "short"), std::abs(size), entry,
            mark, jsonNumber(p, "liq_price"));
        ++synced;
    }
    PULSE_LOG_INFO("app",
        "Position sync complete: {} position(s) imported from exchange",
        synced);
}

/// `pulsetrader cli` — attach to a running engine's control socket.
int runCli(int argc, char *argv[])
{
    ClientArgs args;
    if (!parseClientArgs(argc, argv, args))
    {
        std::cerr << "Usage: pulsetrader cli [--config trading.toml] "
                     "[--host HOST] [--port PORT]\n";
        return 1;
    }

    const auto cfg = loadControlConfig(args);

    pulse::control::ControlClient client;
    if (!client.connect(cfg.control.bindAddress, cfg.control.port))
    {
        std::cerr << "Error: cannot reach engine control socket at "
                  << cfg.control.bindAddress << ":" << cfg.control.port
                  << " — is the trading engine running?\n";
        return 1;
    }

    return runCliRepl(client);
}

/// `pulsetrader mcp` — stdio MCP server bridging to the control socket.
int runMcp(int argc, char *argv[])
{
    ClientArgs args;
    if (!parseClientArgs(argc, argv, args))
    {
        std::cerr << "Usage: pulsetrader mcp [--config trading.toml] "
                     "[--host HOST] [--port PORT]\n";
        return 1;
    }

    const auto cfg = loadControlConfig(args);

    // Stdio hygiene: MCP protocol owns stdout — console logging would
    // corrupt the stream, so force file-only logging BEFORE init.
    pulse::PulseConfig mcp_cfg = cfg;
    mcp_cfg.log.toConsole = false;
    pulse::logging::Logger::init(mcp_cfg.log);

    auto client = std::make_shared<pulse::control::ControlClient>();
    // Connect lazily: initialize/tools/list work even if the engine is
    // down; tools/call surface the unreachable error.
    client->connect(mcp_cfg.control.bindAddress, mcp_cfg.control.port, 1000);

    pulse::control::McpServer::Backend backend =
        [client](const std::string &method, const nlohmann::json &params)
        {
            return client->call(method, params);
        };

    pulse::control::McpServer server(backend);
    server.run(std::cin, std::cout);

    pulse::logging::Logger::shutdown();
    return 0;
}

int main(int argc, char *argv[])
{
    // Strip the program name; subcommand consumers also strip the
    // subcommand token itself (both runTrade and cli/mcp parsers
    // iterate argv from index 0).
    std::string subcommand = "trade";
    if (argc > 1)
    {
        const std::string first(argv[1]);
        if ("trade" == first || "cli" == first || "mcp" == first)
        {
            subcommand = first;
            argv += 2;
            argc -= 2;
        }
        else
        {
            argv += 1;
            argc -= 1;
        }
    }
    else
    {
        argv += 1;
        argc -= 1;
    }

    if ("cli" == subcommand)
    {
        return runCli(argc, argv);
    }
    if ("mcp" == subcommand)
    {
        return runMcp(argc, argv);
    }
    return runTrade(argc, argv);
}
