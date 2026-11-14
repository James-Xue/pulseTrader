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
//   pulsetrader backtest ...             Offline strategy replay (M29)
//   pulsetrader cli                      Remote control REPL
//   pulsetrader mcp                      stdio MCP server

#include "backtest_cli.hpp"
#include "kline_store_cli.hpp"
#include "backtest/DailyKlineSync.hpp"
#include "backtest/GateKlineFetcher.hpp"
#include "backtest/SqliteKlineReader.hpp"
#include "backtest/WarmupSeeder.hpp"
#include "core/SingleInstanceGuard.hpp"
#include "core/config.hpp"
#include "core/config_loader.hpp"
#include "core/config_validator.hpp"
#include "core/snapshot_types.hpp"
#include "core/types.hpp"
#include "exchange/GateRestClient.hpp"
#include "market/SymbolRegistry.hpp"
#include "exchange/GateWsClient.hpp"
#include "logging/Logger.hpp"
#include "market/MarketFeed.hpp"
#include "risk/DrawdownGuard.hpp"
#include "risk/GoldTrader.hpp"
#include "risk/ProtectiveExitManager.hpp"
#include "risk/OrderRateLimiter.hpp"
#include "risk/PositionManager.hpp"
#include "risk/RiskManager.hpp"
#include "execution/OrderExecutor.hpp"
#include "execution/OrderTracker.hpp"
#include "strategy/StrategyManager.hpp"
#include "strategy/StrategyRegistry.hpp"
#include "strategy/signal/SignalAggregator.hpp"
#include "strategy/signal/SignalBoard.hpp"
#include "ai/AiPipeline.hpp"
#include "ai/EngineSnapshotCollector.hpp"
#include "heartbeat/HeartbeatScheduler.hpp"
#include "control/CommandParser.hpp"
#include "control/ControlClient.hpp"
#include "control/EngineServices.hpp"
#include "grid/GridGateway.hpp"
#include "funding/FundingWatch.hpp"
#include "grid/GridManager.hpp"
#include "control/JsonRpcServer.hpp"
#include "control/McpServer.hpp"
#include "control/OrderFlowExecutor.hpp"

#include <fmt/ranges.h>


#ifdef PULSE_ENABLE_SQLITE
#include "trade_recorder/MarketRecorder.hpp"
#include "trade_recorder/SignalRecorder.hpp"
#include "trade_recorder/TradeRecorder.hpp"
#else
// Forward declaration keeps the non-SQLite call sites uniform (null recorder).
namespace pulse::trade_recorder
{
class TradeRecorder;
class SignalRecorder;
}
#endif

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// Console I/O split: POSIX uses poll()/read()/isatty(); Windows waits on the
// console input handle because Winsock's select() only accepts sockets and
// poll() does not exist there. <winsock2.h> must precede <windows.h>.
#if defined(_WIN32)
#include <io.h>
#include <winsock2.h>
#include <windows.h>
#else
#include <poll.h>
#include <unistd.h>
#endif

// ---------------------------------------------------------------------------
// Global stop flag — set by SIGINT / SIGTERM handler
// ---------------------------------------------------------------------------
static std::atomic<bool> g_stop_requested{ false };

static void signalHandler(int /*sig*/)
{
    g_stop_requested.store(true, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Console helpers — platform split for the embedded REPL
// ---------------------------------------------------------------------------
// The REPL waits on stdin with a 200ms timeout instead of blocking, so the
// stop flag and the periodic engine work keep running between keypresses.

/// True when stdin is an interactive terminal (the REPL is enabled only then).
[[nodiscard]] static bool stdinIsTty() noexcept
{
#if defined(_WIN32)
    return 0 != ::_isatty(_fileno(stdin));
#else
    return 0 != isatty(STDIN_FILENO);
#endif
}

/// Wait up to `timeout_ms` for stdin to become readable. Returns > 0 when
/// input is pending, 0 on timeout, < 0 on a wait error (same as poll()).
static int waitStdinReadable(int timeout_ms) noexcept
{
#if defined(_WIN32)
    // A console input handle is signaled once a line is queued (after Enter in
    // the default line-input mode); the same wait also works for a redirected
    // pipe handle, where poll() would have been used on POSIX.
    const HANDLE in = ::GetStdHandle(STD_INPUT_HANDLE);
    if (INVALID_HANDLE_VALUE == in || nullptr == in)
    {
        return -1;
    }
    const DWORD rc = ::WaitForSingleObject(in, static_cast<DWORD>(timeout_ms));
    if (WAIT_OBJECT_0 == rc)
    {
        return 1;
    }
    return (WAIT_TIMEOUT == rc) ? 0 : -1;
#else
    pollfd pfd{ STDIN_FILENO, POLLIN, 0 };
    const int rc = ::poll(&pfd, 1, timeout_ms);
    return (rc > 0 && 0 != (pfd.revents & POLLIN)) ? 1 : rc;
#endif
}

/// Read up to `len` bytes from stdin. Returns the byte count, 0 on EOF, or
/// < 0 on error (same contract as POSIX read()).
static std::ptrdiff_t readStdin(char* buf, std::size_t len) noexcept
{
#if defined(_WIN32)
    return static_cast<std::ptrdiff_t>(
        ::_read(_fileno(stdin), buf, static_cast<unsigned int>(len)));
#else
    return static_cast<std::ptrdiff_t>(::read(STDIN_FILENO, buf, len));
#endif
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static std::string envOr(const char* name, const std::string& fallback)
{
    const char* val = std::getenv(name);
    return (val && val[0]) ? std::string(val) : fallback;
}

// SignalType → lowercase wire string ("buy" / "sell" / "flat") for the
// signal journal; mirrors the SignalBoard entry serialization.
// Only used by the SQLite signal journal, so a build without
// -DPULSE_ENABLE_SQLITE has no caller.
[[maybe_unused]] static const char *
signalTypeWire(pulse::strategy::SignalType type)
{
    switch (type)
    {
    case pulse::strategy::SignalType::Buy:
        return "buy";
    case pulse::strategy::SignalType::Sell:
        return "sell";
    case pulse::strategy::SignalType::Flat:
        return "flat";
    }
    return "flat";
}

// TradingSignal → journal record (M32). Used at both journaling points:
// raw strategy stream (strategy callback) and consensus emissions
// (aggregator output callback, which self-identifies via its strategy id).
#ifdef PULSE_ENABLE_SQLITE
static pulse::trade_recorder::SignalRecord toSignalRecord(
    const pulse::strategy::TradingSignal &sig)
{
    return pulse::trade_recorder::SignalRecord{
        .ts_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                     sig.timestamp.time_since_epoch())
                     .count(),
        .strategy_id = sig.strategy_id,
        .symbol = sig.symbol,
        .market_type = toString(sig.market_type),
        .type = signalTypeWire(sig.type),
        .confidence = sig.confidence,
        .price = sig.price,
        .reason = sig.reason,
        .indicators = sig.indicators.dump(),
    };
}
#endif

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
    pulse::risk::DrawdownGuard* drawdown_guard,
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

    // Drawdown-guard equity aggregation.
    //
    // All configured accounts are summed in USDT-equivalent (USD≈USDT) so a
    // transfer between accounts does not read as a drawdown. The snapshot is
    // fed to the guard only when EVERY configured fetch succeeds: feeding a
    // partial sum after a transient API failure would fake a huge drawdown
    // and halt trading.
    double equity_total = 0.0;
    bool equity_complete = true;

    if (nullptr != rest_client)
    {
        auto bal_result = rest_client->getFuturesAccountBalance();
        if (ok(bal_result))
        {
            const auto &bal = value(bal_result);
            equity_total += bal.total;
            msg << " | futures " << std::fixed << std::setprecision(2)
                << bal.total << " " << bal.currency
                << " (avail " << bal.available
                << ", pnl " << (bal.unrealised_pnl >= 0 ? "+" : "")
                << bal.unrealised_pnl << ")";
        }
        else
        {
            equity_complete = false;
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
                    equity_total += avail + locked;
                    msg << " | spot " << std::fixed << std::setprecision(2)
                        << (avail + locked) << " USDT"
                        << " (avail " << avail << ")";
                    break;
                }
            }
        }
        else
        {
            equity_complete = false;
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
            equity_total += equity;
            msg << " | cfd " << std::fixed << std::setprecision(2)
                << equity << " USD (avail " << free << ")";
        }
        else
        {
            equity_complete = false;
        }
    }

    // Feed the circuit breaker once per heartbeat. updateEquity() also rolls
    // the daily baseline at the UTC day boundary, so no separate reset call is
    // needed. Skip the update entirely when any fetch failed — see above.
    if (nullptr != drawdown_guard)
    {
        if (equity_complete)
        {
            drawdown_guard->updateEquity(equity_total);
        }
        else
        {
            PULSE_LOG_DEBUG("system",
                "Skipping drawdown equity update — an account balance fetch failed");
        }
    }

    PULSE_LOG_INFO("system", "{}", msg.str());

    first_call = false;
}

// ===========================================================================
// main
// ===========================================================================

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
    // The gold trader (L7) is a position supervisor, not a strategy: it
    // needs the CFD feed and executor even when no XAUUSD strategy instance
    // exists. Forcing the market here keeps the startup contract
    // "[gold_trader] enabled = true loads the gold trader" true for
    // strategy-free configurations.
    if (cfg.gold_trader.enabled)
    {
        has_cfd = true;
    }

    // [execution] markets: wire the feed + executor + tracker for a market
    // even when no enabled strategy instance targets it. An external strategy
    // runtime drives PT through the control socket and needs that path to
    // exist before its first open_order ("No executor for market_type"
    // otherwise). Same contract as the gold-trader block above.
    for (const auto &market : cfg.execution.markets)
    {
        if ("spot" == market)
        {
            has_spot = true;
        }
        else if ("futures" == market)
        {
            has_futures = true;
        }
        else if ("cfd" == market)
        {
            has_cfd = true;
        }
    }

    if (!cfg.execution.markets.empty())
    {
        log->info("[L1] Execution markets requested by [execution]: {}",
                  cfg.execution.markets.size());
    }

    // Default to spot if no strategies configured (backward compatibility).
    if (!has_spot && !has_futures && !has_cfd) has_spot = true;

    // Per-market symbol lists for the feeds. Each feed must only poll the
    // symbols that exist in ITS market: the CFD feed polling a futures-only
    // symbol (e.g. BTC_USDT) spams SYMBOL_NOT_EXISTS every second AND fails
    // the whole SymbolRegistry detail load (losing XAUUSD's quanto_multiplier
    // = 100 → notional underestimated 100x). Falls back to cfg.symbols when
    // a market has no enabled strategies (backward compatibility).
    auto symbols_for = [&cfg](MarketType mt)
    {
        std::vector<std::string> syms;
        for (const auto &inst : cfg.strategy.strategies)
        {
            if (!inst.enabled || inst.market_type != mt)
            {
                continue;
            }
            if (std::find(syms.begin(), syms.end(), inst.symbol) == syms.end())
            {
                syms.push_back(inst.symbol);
            }
        }

        // Watchlist symbols are streamed in the ACTIVE market only. A name
        // can be foreign to the other venues (XAU_USDT is futures-only), and
        // a feed polling a symbol it does not know logs an error every tick.
        if (mt == cfg.active_market)
        {
            for (const auto &symbol : cfg.watch_symbols)
            {
                if (std::find(syms.begin(), syms.end(), symbol) == syms.end())
                {
                    syms.push_back(symbol);
                }
            }
        }

        // A market wired by [execution] with no strategy instance needs its
        // symbols from somewhere: [execution].symbols wins over the global
        // symbol list, because the global list may contain names that do not
        // exist on this venue (the same hazard the CFD note below describes).
        const bool execution_market =
            std::find(cfg.execution.markets.begin(),
                      cfg.execution.markets.end(),
                      std::string{ toString(mt) }) != cfg.execution.markets.end();
        if (syms.empty() && execution_market && !cfg.execution.symbols.empty())
        {
            return cfg.execution.symbols;
        }

        // The gold trader manages a CFD symbol outside the strategy list.
        // Add it explicitly and do NOT fall back to the global symbol list
        // (a futures-only name in the CFD feed fails the whole registry
        // detail load and drops XAUUSD's contract multiplier).
        if (MarketType::Cfd == mt && cfg.gold_trader.enabled)
        {
            if (std::find(syms.begin(), syms.end(), cfg.gold_trader.symbol)
                == syms.end())
            {
                syms.push_back(cfg.gold_trader.symbol);
            }
            return syms;
        }

        return syms.empty() ? cfg.symbols : syms;
    };
    // A market with no feed has no symbols to fetch. symbols_for() falls back
    // to the mixed cfg.symbols list when a market has no enabled strategies
    // (backward compatibility), which then hands the daily kline sync names
    // that do not exist on that venue: a spot sync for the futures-only
    // SNDK_USDT returned INVALID_CURRENCY_PAIR every day (2026-11-14).
    // Gating on has_* makes an unwired market genuinely empty.
    const auto spot_symbols = has_spot
        ? symbols_for(MarketType::Spot) : std::vector<std::string>{};
    const auto futures_symbols = has_futures
        ? symbols_for(MarketType::Futures) : std::vector<std::string>{};
    const auto cfd_symbols = has_cfd
        ? symbols_for(MarketType::Cfd) : std::vector<std::string>{};

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

    // Balance-only REST clients for markets with no strategies configured.
    // The account snapshot must reflect every market the key can reach: a
    // spot-only strategy list used to leave futures_rest/cfd_rest null, so
    // get_account reported a spot balance and nothing else — the dashboard had
    // no futures or CFD block to draw. This mirrors the spot fallback above
    // ("balance queries only"). No feed, no WS client, no executor is created
    // here; these clients exist purely for account queries (every 30 s at most
    // per market in the Qt client), and a rejected call just omits that block.
    if (!has_futures)
    {
        futures_rest = std::make_unique<pulse::exchange::GateRestClient>(
            cfg.exchange, MarketType::Futures);
        log->info("[L1] Futures REST client created (balance queries only)");
    }

    if (!has_cfd)
    {
        cfd_rest = std::make_unique<pulse::exchange::GateRestClient>(
            cfg.exchange, MarketType::Cfd);
        log->info("[L1] CFD REST client created (balance queries only)");
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

// Uniform raw pointer for the AI snapshot collector / EngineServices call
// sites (null in non-SQLite builds).
#ifdef PULSE_ENABLE_SQLITE
    pulse::trade_recorder::TradeRecorder *trade_recorder_ptr = trade_recorder.get();
#else
    pulse::trade_recorder::TradeRecorder *trade_recorder_ptr = nullptr;
#endif

#ifdef PULSE_ENABLE_SQLITE
    // Market data recorder (M18): persists ticker/kline to the same SQLite
    // file via a second connection (WAL). Failure is warn-only — the engine
    // keeps running without market data persistence.
    std::unique_ptr<pulse::trade_recorder::MarketRecorder> market_recorder;

    if (cfg.sqlite.recordMarketData)
    {
        auto mr_result = pulse::trade_recorder::MarketRecorder::open(
            cfg.sqlite.dbPath, 8192, 128, std::chrono::milliseconds(1000),
            cfg.sqlite.skipKlineMarkets);

        if (pulse::ok(mr_result))
        {
            market_recorder = std::move(pulse::value(mr_result));
            // Wire the sink to all feeds (constructed above at L1); events
            // only flow once each feed is started.
            if (spot_feed)
            {
                spot_feed->setMarketDataSink(market_recorder.get());
            }
            if (futures_feed)
            {
                futures_feed->setMarketDataSink(market_recorder.get());
            }
            if (cfd_feed)
            {
                cfd_feed->setMarketDataSink(market_recorder.get());
            }
            log->info("[L8+] Market data recorder opened: '{}'",
                      cfg.sqlite.dbPath);
        }
        else
        {
            log->warn("[L8+] Market data recorder failed to open: {}",
                      pulse::error(mr_result).message);
        }
    }
    else
    {
        log->info("[L8+] Market data recording disabled "
                  "(set sqlite.record_market = true)");
    }

    // M32 signal journal: appends every raw strategy signal (pre-aggregation,
    // regardless of the auto_trade gate) plus aggregator consensus rows to the
    // `signals` table — the replay source for the A/B weight trial. Third WAL
    // connection to the same file; failure is warn-only.
    std::unique_ptr<pulse::trade_recorder::SignalRecorder> signal_recorder;

    if (cfg.sqlite.enabled && cfg.sqlite.recordSignals)
    {
        auto sr_result = pulse::trade_recorder::SignalRecorder::open(
            cfg.sqlite.dbPath);

        if (pulse::ok(sr_result))
        {
            signal_recorder = std::move(pulse::value(sr_result));
            log->info("[L8+] Signal journal opened: '{}'",
                      cfg.sqlite.dbPath);
        }
        else
        {
            log->warn("[L8+] Signal journal failed to open: {}",
                      pulse::error(sr_result).message);
        }
    }
    else
    {
        log->info("[L8+] Signal journal disabled "
                  "(set sqlite.record_signals = true)");
    }
#endif

    // Uniform raw pointer for the unconditional signal callbacks (null in
    // non-SQLite builds).
#ifdef PULSE_ENABLE_SQLITE
    pulse::trade_recorder::SignalRecorder *signal_recorder_ptr =
        signal_recorder.get();
#else
    pulse::trade_recorder::SignalRecorder *signal_recorder_ptr = nullptr;
#endif

    // ------------------------------------------------------------------
    // 7. L6: Strategy Engine
    // ------------------------------------------------------------------
    pulse::strategy::StrategyManager strategy_mgr;
    pulse::strategy::SignalAggregator aggregator(cfg.strategy);

    // Name-keyed registry with fallback: unknown names become a passive
    // UnifiedScalper (never emits) instead of being skipped.
    auto strategy_registry = pulse::strategy::makeBuiltinStrategyRegistry();

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

        // Registry fallback: never returns nullptr (unknown names become a
        // passive UnifiedScalper — see StrategyRegistry::create).
        auto strat = strategy_registry.create(inst_cfg.name, ctx);

        // Seed the per-instance min_confidence from config into the live
        // params. Previously the config value was loaded but never applied,
        // so every strategy ran with the hard-coded default (0.6) — a scale
        // mismatch that silently muted low-confidence-scale strategies on the
        // signal board (momentum ~1e-4, mean_reversion ~0.01-0.1; only
        // supertrend, whose confidence is distance/ATR clamped to 1.0, ever
        // published — 2026-08-17 overnight finding).
        strat->params().min_confidence.store(inst_cfg.min_confidence,
                                             std::memory_order_release);

        // Seed the per-instance order_quantity from config into the live
        // params. Previously only min_confidence was seeded, so
        // get_strategy_params showed the hard-coded default (0.001) while
        // automatic orders actually used the config value — a display
        // mismatch that also made live tuning of order_quantity appear
        // ineffective. Same bug class as the min_confidence seeding above.
        strat->params().order_quantity.store(inst_cfg.order_quantity,
                                             std::memory_order_release);

        // Seed the per-strategy auto-trade gate from the global signal_only
        // config: signal_only=true → every strategy starts in signals-only
        // mode (auto_trade=0); false → auto-trading on. The runtime one-click
        // switch (set_strategy_trading / `autotrade <id> on|off`) overrides
        // this per strategy; a restart returns to this fail-safe default.
        strat->params().auto_trade.store(cfg.strategy.signal_only ? 0.0 : 1.0,
                                         std::memory_order_release);

        log->info("[L6] Registered strategy: {} on {} (qty={}, conf={:.2f}, market={})",
                  inst_cfg.name, inst_cfg.symbol,
                  inst_cfg.order_quantity, inst_cfg.min_confidence,
                  pulse::toString(inst_cfg.market_type));

        strategy_mgr.registerStrategy(std::move(strat));
    }

    // A strategy-free engine is valid when the gold trader is enabled: the
    // supervisor manages positions that may have been opened manually.
    if (0 == strategy_mgr.strategyCount() && !cfg.gold_trader.enabled)
    {
        log->error("No strategies registered. Exiting.");
        pulse::logging::Logger::shutdown();
        return 1;
    }

    // Signal board: every raw signal (with its indicator snapshot) and every
    // consolidated aggregator output is published here for the control plane
    // (`get_signals`) and external consumers. Pure observation — the board
    // never triggers execution.
    auto board = std::make_shared<pulse::strategy::SignalBoard>(
        cfg.strategy.signal_aggregator_threshold);

    // Wire: strategy signals → signal board + aggregator.
    //
    // Per-strategy auto-trade gate lives HERE (not in OrderFlowExecutor):
    // the aggregator output loses the originating strategy id, so a strategy
    // with auto_trade=0 (signals-only) must be stopped before aggregation.
    // It still publishes to the board (get_signals stays live for testing).
    // Flat exits (B1): the aggregator ignores Flat by design, so the signal
    // callback cannot route them. Strategy threads push them here and the
    // main loop closes the matching positions, where the CFD close endpoint
    // (which is not an order) is reachable.
    std::mutex flat_mutex;
    std::vector<pulse::strategy::TradingSignal> pending_flats;

    strategy_mgr.setSignalCallback(
        [&aggregator, board, &strategy_mgr,
         signal_recorder_ptr, &flat_mutex, &pending_flats](const pulse::strategy::TradingSignal& sig)
        {
            board->publish(sig);
            // M32 journal: raw stream is recorded BEFORE the auto_trade gate
            // so counterfactual weight policies can be replayed offline.
#ifdef PULSE_ENABLE_SQLITE
            if (signal_recorder_ptr)
            {
                signal_recorder_ptr->record(toSignalRecord(sig));
            }
#endif
            const auto *params = strategy_mgr.paramsByName(sig.strategy_id);
            if (nullptr == params
                || params->auto_trade.load(std::memory_order_acquire) != 0.0)
            {
                if (pulse::strategy::SignalType::Flat == sig.type)
                {
                    // Exit intent — bypass the directional aggregator.
                    std::lock_guard flat_lock(flat_mutex);
                    pending_flats.push_back(sig);
                }
                else
                {
                    aggregator.addSignal(sig);
                }
            }
        });

    log->info("[L6] Strategy engine ready ({} instances, threshold={:.2f}, "
              "cooldown={}s)",
              strategy_mgr.strategyCount(),
              cfg.strategy.signal_aggregator_threshold,
              cfg.strategy.signal_cooldown_sec);

    // ------------------------------------------------------------------
    // 8. L4 + L5: AI Pipeline + Heartbeat
    // ------------------------------------------------------------------
    // Param-change audit log shared by the AI pipeline and the manual
    // control-plane channel (get_param_history).
    pulse::core::ParamChangeLog param_change_log;

    pulse::ai::AiPipeline ai_pipeline(cfg.ai, cfg.twitter, cfg.news,
                                      nullptr, &param_change_log);

    std::unique_ptr<pulse::heartbeat::HeartbeatScheduler> heartbeat;
    if (cfg.ai.heartbeatIntervalSec > 0 && !cfg.ai.apiKey.empty())
    {
        // Wire AI to each strategy's actual handles (identity + params) and
        // a live snapshot collector (real feeds + per-strategy performance
        // from the trades DB when SQLite is enabled).
        auto handles = strategy_mgr.allHandles();
        pulse::ai::EngineSnapshotCollector collector(
            spot_feed.get(), futures_feed.get(), cfd_feed.get(), handles,
            trade_recorder_ptr,
            static_cast<std::int64_t>(cfg.ai.stats_lookback_hours) * 3600LL
                * 1000'000'000LL);
        pulse::ai::SnapshotProvider provider = [&collector]()
        {
            return collector.collect();
        };
        heartbeat = std::make_unique<pulse::heartbeat::HeartbeatScheduler>(
            cfg.ai, ai_pipeline, std::move(handles), std::move(provider));
        log->info("[L5] Heartbeat scheduler created (interval: {}s, {} strategy handles)",
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
        spot_feed ? &spot_feed->orderbookManager() : nullptr,
        futures_feed ? &futures_feed->orderbookManager() : nullptr,
        rest_mutex
#ifdef PULSE_ENABLE_SQLITE
        , trade_recorder.get()
#else
        , nullptr
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
        // Use the CFD feed's own symbol set, not the global cfg.symbols: the
        // detail endpoint is queried per symbol and one foreign (futures-only)
        // name can fail the whole load, which would leave the gold trader
        // without XAUUSD's contract volume. symbols_for(Cfd) already includes
        // the gold trader's symbol and the CFD watchlist.
        pulse::market::SymbolRegistry cfd_registry(*cfd_rest,
                                                   pulse::MarketType::Cfd);
        if (cfd_registry.loadFromRest(cfd_symbols))
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

    aggregator.setOutputCallback(
        [&order_flow, board,
         signal_recorder_ptr](const pulse::strategy::TradingSignal& sig)
        {
            // Published in BOTH modes: in signal-only mode this is the only
            // observable trace of the aggregator consensus.
            board->publishAggregate(sig);
            // M32 journal: consensus rows self-identify via their strategy id
            // ("signal_aggregator") and are excluded from replay inputs; they
            // exist so a later auto-trade run can cross-check replay fidelity.
#ifdef PULSE_ENABLE_SQLITE
            if (signal_recorder_ptr)
            {
                signal_recorder_ptr->record(toSignalRecord(sig));
            }
#endif
            // In signal-only mode onSignal is a no-op (gate inside).
            order_flow.onSignal(sig);
        });

    // Completion callbacks are wired by OrderFlowExecutor's constructor
    // (consume reservation → open/close position → drawdown PnL → SQLite).


    // ------------------------------------------------------------------
    // 11b. Control plane: EngineServices + JSON-RPC control socket
    // ------------------------------------------------------------------
    const auto engine_start_ref = std::chrono::steady_clock::now();
    // M27 engine-native grid service: exchange-facing gateway + state machine.
    // Starts disabled; `grid_start` (REPL/MCP) activates it. The gateway
    // forwards to placeManualOrder (full risk gate) and the futures REST
    // client (exchange-order truth view). With `[grid] enabled = true` and a
    // saved Running state (schema 2) the grid resumes automatically on boot —
    // the crash-survival point of the engine-native grid. To take over
    // manually: `grid pause` BEFORE stopping the engine (the pause is
    // persisted); legacy state files (schema 1) restore as Paused.
    pulse::grid::GridGateway grid_gateway{
        order_flow, futures_rest.get(), position_mgr, rest_mutex };
    pulse::grid::GridManager grid_mgr{
        cfg.grid, grid_gateway, *board, futures_feed.get(), rest_mutex,
        cfg.log.logDir.empty() ? std::filesystem::path{ "data" }
                               : std::filesystem::path{ cfg.log.logDir } };
    if (cfg.grid.enabled)
    {
        grid_mgr.loadState();
    }

    // Funding-window monitor: pure observation (signal board + logs), never
    // trades. Polls the public funding endpoint on its own slow gate; only
    // exists when a futures REST client is live.
    std::unique_ptr<pulse::funding::FundingWatch> funding_watch;
    if (cfg.funding_watch.enabled && futures_rest)
    {
        funding_watch = std::make_unique<pulse::funding::FundingWatch>(
            cfg.funding_watch, *board, *futures_rest, rest_mutex);
        log->info("[L8+] FundingWatch armed ({} symbols, threshold={}, "
                  "consec={}×8h)",
                  cfg.funding_watch.symbols.size(),
                  cfg.funding_watch.threshold,
                  cfg.funding_watch.consec_events);
    }

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
        *board,
        rest_mutex,
        symbol_registry,
        &grid_mgr,
        &param_change_log,
        trade_recorder_ptr);

    // Reconcile positions that already exist on the exchange (previous
    // engine run, manual trading) so risk limits and displays reflect the
    // true exposure from the first second. Non-fatal on failure. The same
    // code path also runs every ~10s (position sync tick below) and on
    // demand via `sync_positions` / REPL `sync`.
    (void)services.syncPositions();

    // L7 protective exits (audit B1): engine-side stop-loss / take-profit
    // supervisor. Exchange-side SL/TP orders are not attached to futures
    // entries yet, so this is the only backstop while the process is alive.
    //
    // Prices come from the per-market ticker caches; a missing quote keeps
    // the previous mark (no fabricated triggers). Closes go through
    // EngineServices so CFD uses its dedicated close endpoint while
    // futures/spot go through the risk gate as reduce-only orders.
    //
    // When the gold trader arms, it becomes the SOLE exit authority for its
    // symbol: the generic percentage trail/take-profit ladder below is not
    // part of the USD dynamic-stop policy and would otherwise partially
    // close a managed position (silently rescaling its USD thresholds).
    // Positions opened by an external caller-authoritative runtime (GT) are
    // excluded from the percentage trail/take-profit ladders ONLY while the
    // exchange-native floor the design relies on actually exists: a CFD entry
    // carrying sl_price. On futures/spot open_order rejects sl_price/tp_price,
    // so an "external" position there has no exchange-side stop at all — if
    // this filter skipped it, a live position would be left with no stop
    // anywhere. The same holds for a CFD order that omitted the optional
    // sl_price. Those positions fall through to the generic manager (the
    // 0.5% trailing stop this engine had before external ownership existed).
    // `gold_armed` is set right after a successful construction; both the
    // filter and the manager run only on the main loop.
    bool gold_armed = false;
    pulse::risk::ProtectiveExitManager protective_exits(
        cfg.risk,
        position_mgr,
        [&spot_feed, &futures_feed, &cfd_feed](const pulse::Symbol &symbol,
                                               pulse::MarketType market_type)
            -> std::optional<pulse::Price>
        {
            pulse::market::MarketFeed *feed = nullptr;
            switch (market_type)
            {
            case pulse::MarketType::Futures:
                feed = futures_feed.get();
                break;
            case pulse::MarketType::Cfd:
                feed = cfd_feed.get();
                break;
            default:
                feed = spot_feed.get();
                break;
            }
            if (nullptr == feed)
            {
                return std::nullopt;
            }
            const auto ticker = feed->tickerCache().get(symbol);
            if (!ticker.has_value() || ticker->last <= 0.0)
            {
                return std::nullopt;
            }
            return ticker->last;
        },
        [&services](const pulse::risk::Position &pos, double quantity)
        {
            nlohmann::json close_params;
            close_params["position_id"] = pos.position_id;
            close_params["quantity"] = quantity;
            return pulse::ok(services.closePosition(close_params));
        },
        [&gold_armed, &cfg](const pulse::risk::Position &pos)
        {
            // Caller exit-ownership wins over the gold gate only when the
            // exchange-native stop is attached: a CFD position with sl_price
            // > 0 is protected even if this process dies, so its owner's exit
            // logic is authoritative. Without that floor (any futures/spot
            // external position, or a CFD entry with no sl_price) skipping PT
            // would leave the position unprotected, so PT keeps managing it.
            if ("external" == pos.origin && pos.sl_price > 0.0
                && pulse::MarketType::Cfd == pos.market_type)
            {
                return false;
            }
            return !(gold_armed && pos.symbol == cfg.gold_trader.symbol
                     && pos.market_type == cfg.gold_trader.market_type);
        });

    // L7 gold trader (strategy 1: USD dynamic stop ladder). This supervisor
    // is deliberately separate from ProtectiveExitManager: it works in
    // absolute USD thresholds (8 / 2 / 5) instead of percentage-of-price, it
    // compares against the 1-minute SuperTrend, and it mirrors every
    // tightened stop to the exchange-native price_sl so a process crash does
    // not leave the position unprotected.
    //
    // It manages every open XAUUSD CFD position, including positions opened
    // manually in the Gate app and imported by sync_positions. A missing
    // quote or kline buffer skips the pass rather than fabricating a level.
    // Fail closed: the USD ladder is meaningless without the symbol's
    // contract volume. The registry lookup is the only place that can prove
    // XAUUSD is 100 oz/lot before the supervisor starts pushing stops; a
    // missing/mismatched entry disables the gold trader instead of silently
    // placing stops 100x too tight.
    bool gold_metadata_ok = true;
    if (cfg.gold_trader.enabled)
    {
        gold_metadata_ok = false;

        if (symbol_registry)
        {
            const auto gold_info =
                symbol_registry->get(cfg.gold_trader.symbol);
            if (gold_info.has_value()
                && pulse::MarketType::Cfd == gold_info->market_type
                && std::abs(gold_info->quanto_multiplier
                            - cfg.gold_trader.contract_volume)
                       <= cfg.gold_trader.contract_volume * 0.01)
            {
                gold_metadata_ok = true;
            }
            else
            {
                log->error("[L7] Gold trader disabled: {} metadata "
                           "quanto_multiplier={} (expected {} oz/lot)",
                           cfg.gold_trader.symbol,
                           gold_info.has_value()
                               ? gold_info->quanto_multiplier
                               : 0.0,
                           cfg.gold_trader.contract_volume);
            }
        }
        else
        {
            log->error("[L7] Gold trader disabled: no CFD symbol registry "
                       "for {}", cfg.gold_trader.symbol);
        }
    }

    std::unique_ptr<pulse::risk::GoldTrader> gold_trader;
    if (cfg.gold_trader.enabled && gold_metadata_ok)
    {
        const double st_period = cfg.gold_trader.supertrend_period;
        const double st_multiplier = cfg.gold_trader.supertrend_multiplier;

        gold_trader = std::make_unique<pulse::risk::GoldTrader>(
            cfg.gold_trader,
            position_mgr,
            [&cfd_feed](const pulse::Symbol &symbol) -> std::optional<pulse::Price>
            {
                if (!cfd_feed)
                {
                    return std::nullopt;
                }
                const auto ticker = cfd_feed->tickerCache().get(symbol);
                if (!ticker.has_value() || ticker->last <= 0.0)
                {
                    return std::nullopt;
                }
                return ticker->last;
            },
            [&cfd_feed, st_period, st_multiplier](const pulse::Symbol &symbol)
                -> std::optional<pulse::risk::SuperTrendPoint>
            {
                if (!cfd_feed)
                {
                    return std::nullopt;
                }

                // The CFD feed returns the still-forming candle and marks it
                // closed (audit B6). Trust a bar only when a successful kline
                // fetch STARTED after its close_time: that fetch necessarily
                // re-read and upserted the bar in its final form. A bar from
                // the partial first sighting is never fed to SuperTrend —
                // tighten-only would otherwise adopt a spuriously tight band
                // permanently.
                const auto last_fetch_ms =
                    cfd_feed->lastKlineFetchMs(symbol);
                if (!last_fetch_ms.has_value())
                {
                    return std::nullopt;
                }

                // The buffer capacity is 500; validating a period up to 499
                // keeps the whole window available. A smaller window here
                // would silently disable the SuperTrend leg for large
                // configured periods.
                const auto candles =
                    cfd_feed->getKlineBuffer(symbol).snapshot(500);

                std::vector<double> highs;
                std::vector<double> lows;
                std::vector<double> closes;
                highs.reserve(candles.size());
                lows.reserve(candles.size());
                closes.reserve(candles.size());

                for (const auto &candle : candles)
                {
                    if (!candle.closed || candle.open_time <= 0
                        || candle.close_time > last_fetch_ms.value())
                    {
                        continue;
                    }
                    highs.push_back(candle.high);
                    lows.push_back(candle.low);
                    closes.push_back(candle.close);
                }

                return pulse::risk::SuperTrend::compute(
                    highs, lows, closes,
                    static_cast<std::size_t>(st_period), st_multiplier);
            },
            [&position_mgr, &services](const pulse::risk::Position &pos,
                                       pulse::Price mark)
            {
                // Refresh the risk view first: the realized PnL recorded by
                // the close uses Position::current_price, which otherwise only
                // moves on the ~10 s exchange sync.
                position_mgr.updatePrice(pos.position_id, mark);
                nlohmann::json close_params;
                close_params["position_id"] = pos.position_id;
                return pulse::ok(services.closePosition(close_params));
            },
            [&services](const pulse::risk::Position &pos, pulse::Price sl_price)
            {
                nlohmann::json stop_params;
                stop_params["position_id"] = pos.position_id;
                stop_params["sl_price"] = sl_price;
                return pulse::ok(services.modifySlTp(stop_params));
            });

        // The gold trader is now the sole exit authority for its symbol; the
        // generic ProtectiveExitManager filter above starts skipping it.
        gold_armed = true;

        log->info("[L7] Gold trader armed: {} {} hard={:.2f}U breakeven={:.2f}U "
                  "peak-drawdown={:.2f}U supertrend=1m(period {:.0f}, mult {:.1f}) "
                  "buffer={:.2f}U poll={}ms exchange_sl={}",
                  cfg.gold_trader.symbol,
                  pulse::toString(cfg.gold_trader.market_type),
                  cfg.gold_trader.hard_stop_usd,
                  cfg.gold_trader.breakeven_trigger_usd,
                  cfg.gold_trader.peak_drawdown_usd,
                  cfg.gold_trader.supertrend_period,
                  cfg.gold_trader.supertrend_multiplier,
                  cfg.gold_trader.supertrend_buffer_usd,
                  cfg.gold_trader.poll_interval_ms,
                  cfg.gold_trader.exchange_stop ? "on" : "off");
    }

    // Expose the supervisor to the control plane (get_gold_trader / MCP /
    // Qt status card). A null trader means it is disabled or failed the
    // metadata check; the reason is reported so the UI can explain it.
    std::string gold_disabled_reason;
    if (!cfg.gold_trader.enabled)
    {
        gold_disabled_reason = "disabled by config";
    }
    else if (!gold_metadata_ok)
    {
        gold_disabled_reason = "XAUUSD contract metadata unavailable";
    }
    services.setGoldTrader(gold_trader.get(), cfg.gold_trader,
                           std::move(gold_disabled_reason));

    // The registry is copied into the server; the local copy is used by
    // the embedded REPL (both dispatch to the same EngineServices).
    auto control_registry = pulse::control::makeMethodRegistry(services);
    std::unique_ptr<pulse::control::JsonRpcServer> control_server;
    if (cfg.control.enabled)
    {
        control_server = std::make_unique<pulse::control::JsonRpcServer>(
            cfg.control.bindAddress, cfg.control.port, control_registry);

        // Transport security (M-Q4). Defaults in ControlConfig keep the
        // socket plaintext and unauthenticated, so an untouched config is
        // unchanged.
        pulse::control::JsonRpcServer::Security security;
        security.tlsEnabled = cfg.control.tlsEnabled;
        security.certPath = cfg.control.tlsCertPath;
        security.keyPath = cfg.control.tlsKeyPath;
        security.caPath = cfg.control.tlsCaPath;
        security.requireClientCert = cfg.control.tlsRequireClientCert;
        security.token = cfg.control.token;
        control_server->setSecurity(std::move(security));

        // Insecure-but-expressible combinations are warnings, not errors: the
        // validator only rejects configurations that cannot start at all.
        const bool loopback = ("127.0.0.1" == cfg.control.bindAddress
                               || "::1" == cfg.control.bindAddress
                               || "localhost" == cfg.control.bindAddress);
        if (!loopback && !cfg.control.tlsEnabled && cfg.control.token.empty())
        {
            PULSE_LOG_WARN("control",
                           "control socket is exposed on {} with neither TLS "
                           "nor a token — anyone who can reach the port can "
                           "place orders",
                           cfg.control.bindAddress);
        }
        if (!cfg.control.token.empty() && !cfg.control.tlsEnabled)
        {
            PULSE_LOG_WARN("control",
                           "control token is sent over plaintext — use TLS or "
                           "a tunnel");
        }
        if (cfg.control.tlsEnabled && cfg.control.token.empty())
        {
            PULSE_LOG_WARN("control",
                           "control TLS is enabled without a token — any local "
                           "process can still drive the engine");
        }
    }

    // Push CLOSED klines to external strategy runtimes subscribed over the
    // control socket (subscribe_market). The listener runs on the feed I/O
    // thread and only enqueues into bounded per-subscriber queues, so it can
    // never block the market-data hot path. Feeds are stopped explicitly
    // before the server is destroyed (see the shutdown section), so the raw
    // server pointer captured here cannot dangle.
    if (control_server)
    {
        const auto kline_listener = [server = control_server.get()](
                                        const pulse::Symbol &symbol,
                                        pulse::MarketType market_type,
                                        const pulse::market::Kline &kline)
        {
            const std::string market_label = pulse::toString(market_type);
            nlohmann::json params;
            params["symbol"] = symbol;
            params["market_type"] = market_label;
            params["open_time"] = kline.open_time;
            params["close_time"] = kline.close_time;
            params["open"] = kline.open;
            params["high"] = kline.high;
            params["low"] = kline.low;
            params["close"] = kline.close;
            params["volume"] = kline.volume;
            params["closed"] = kline.closed;
            server->publish({ "kline", symbol, market_label },
                            "market.kline", params);
        };
        if (spot_feed)    { spot_feed->setKlineListener(kline_listener); }
        if (futures_feed)
        {
            futures_feed->setKlineListener(kline_listener);
        }
        if (cfd_feed)     { cfd_feed->setKlineListener(kline_listener); }

        // Push order lifecycle events to sessions subscribed over the control
        // socket (subscribe_orders → order.update). Socket-only by design: MCP
        // is request/response and has no server-push channel, so an MCP client
        // still polls get_orders.
        //
        // Two sources feed the same channel:
        //   1. "submitted" — OrderFlowExecutor emits it from inside
        //      placeManualOrder(), right after the exchange accepts the order
        //      and BEFORE the tracker's immediate fill poll (EngineServices
        //      serializes it). That ordering is what makes `submitted` precede
        //      `completed` for market orders that fill on submission.
        //   2. "completed" — OrderFlowExecutor forwards every terminal
        //      ExecutionReport with the reporting tracker's OWN market type
        //      (never inferred from the symbol: BTC_USDT trades on both spot
        //      and futures, and the Topic filter is per market).
        services.setOrderEventSink(
            [server = control_server.get()](const nlohmann::json &event)
            {
                server->publish(
                    { "order",
                      event.value("symbol", std::string{}),
                      event.value("market_type", std::string{}) },
                    "order.update", event);
            });
        order_flow.setOrderUpdateCallback(
            [server = control_server.get()](
                pulse::MarketType market_type,
                const pulse::execution::ExecutionReport &report)
            {
                nlohmann::json event;
                event["event"] = "completed";
                event["order_id"] = report.order_id;
                event["client_order_id"] = report.client_order_id;
                event["symbol"] = report.symbol;
                event["market_type"] = pulse::toString(market_type);
                event["side"] = (pulse::Side::Buy == report.side)
                                    ? "buy" : "sell";
                event["type"] = pulse::toString(report.type);
                event["requested_quantity"] = report.requested_qty;
                event["filled_quantity"] = report.filled_qty;
                event["avg_fill_price"] = report.avg_fill_price;
                event["fees"] = report.fees;
                event["status"] = pulse::execution::orderStatusToString(
                    report.final_status);
                event["ts"] = std::chrono::duration_cast<
                                  std::chrono::milliseconds>(
                                  pulse::now().time_since_epoch())
                                  .count();
                server->publish(
                    { "order", report.symbol, pulse::toString(market_type) },
                    "order.update", event);
            });
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
    // "futures only for now"). Every other direction's strategies pause so they
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

    // Shared kline DB reader (M30 warmup preload + M31 daily sync share one
    // SqliteKlineReader instance — no extra connection). SqliteKlineReader
    // opens lazily; a missing DB degrades to pure API.
    pulse::backtest::SqliteKlineReader *kline_db = nullptr;
#ifdef PULSE_ENABLE_SQLITE
    std::unique_ptr<pulse::backtest::SqliteKlineReader> kline_db_owner;
    if (cfg.sqlite.enabled)
    {
        kline_db_owner =
            std::make_unique<pulse::backtest::SqliteKlineReader>(cfg.sqlite.dbPath);
        kline_db = kline_db_owner.get();
    }
#endif

    // L1.5: Startup kline preload (M30) — seed recent history into each
    // feed's KlineBuffer BEFORE feed->start(): the KlineBuffer assumes a
    // single writer, and the WS I/O thread begins writing once the feed
    // starts. With history in the buffer, each strategy thread's first poll
    // completes warmup instantly (EmaResonance needs 201 candles ≈ 3.4h)
    // instead of accumulating live pushes. The seeder never throws and never
    // writes back to kline_bars; a failed symbol just falls back to live
    // warmup. CFD is skipped — its REST poll loop already backfills 500
    // candles on first poll (MarketFeed::pollLoop).
    if (cfg.strategy.preloadKlines)
    {
        std::size_t seeded_total = 0;
        if (spot_feed && spot_rest)
        {
            pulse::backtest::GateKlineFetcher api_fetcher(*spot_rest);
            pulse::backtest::WarmupSeeder seeder(kline_db, api_fetcher);
            seeded_total += seeder.seed(*spot_feed, spot_symbols, pulse::MarketType::Spot);
        }
        if (futures_feed && futures_rest)
        {
            pulse::backtest::GateKlineFetcher api_fetcher(*futures_rest);
            pulse::backtest::WarmupSeeder seeder(kline_db, api_fetcher);
            seeded_total += seeder.seed(*futures_feed, futures_symbols, pulse::MarketType::Futures);
        }
        log->info("[L1.5] warmup preload done — {} candles seeded", seeded_total);
    }

    // M31: daily kline archival sync. Pulls ~10000 1m candles (Gate REST
    // depth limit) per spot/futures symbol + 500 gold candles (CFD) once per
    // UTC day (Beijing 08:00), merged into kline_bars. Runs on its own
    // worker thread; tick() only does a day-key check. The fetchers below
    // must outlive kline_sync (they are held by reference).
    std::unique_ptr<pulse::backtest::GateKlineFetcher> spot_kline_api;
    std::unique_ptr<pulse::backtest::GateKlineFetcher> futures_kline_api;
    std::unique_ptr<pulse::backtest::CfdKlineSource> cfd_kline_api;
    std::unique_ptr<pulse::backtest::DailyKlineSync> kline_sync;
    if (cfg.sqlite.enabled && cfg.sqlite.dailyKlineSync && spot_rest && futures_rest)
    {
        // The spot/futures REST endpoints reject CFD-only symbols
        // (XAUUSD/XAGUSD → INVALID_CURRENCY_PAIR). symbols_for() falls back
        // to the mixed cfg.symbols list when a market has no enabled
        // strategies, so strip any symbol the CFD market owns.
        auto strip_cfd_symbols = [&cfd_symbols](std::vector<std::string> syms)
        {
            syms.erase(std::remove_if(syms.begin(), syms.end(),
                          [&cfd_symbols](const std::string &s)
                          {
                              return std::find(cfd_symbols.begin(),
                                               cfd_symbols.end(), s)
                                  != cfd_symbols.end();
                          }),
                       syms.end());
            return syms;
        };
        const auto sync_spot_symbols = strip_cfd_symbols(spot_symbols);
        const auto sync_futures_symbols = strip_cfd_symbols(futures_symbols);

        spot_kline_api =
            std::make_unique<pulse::backtest::GateKlineFetcher>(*spot_rest);
        futures_kline_api =
            std::make_unique<pulse::backtest::GateKlineFetcher>(*futures_rest);
        if (cfd_rest)
        {
            cfd_kline_api =
                std::make_unique<pulse::backtest::CfdKlineSource>(*cfd_rest);
        }
        kline_sync = std::make_unique<pulse::backtest::DailyKlineSync>(
            kline_db, *spot_kline_api, *futures_kline_api, cfd_kline_api.get(),
            rest_mutex, sync_spot_symbols, sync_futures_symbols, cfd_symbols,
            cfg.sqlite.dailySyncHour);
        log->info("[M31] daily kline sync armed (hour {}+08, spot={} futures={} cfd={})",
                  cfg.sqlite.dailySyncHour, sync_spot_symbols.size(),
                  sync_futures_symbols.size(), cfd_symbols.size());
    }

    // L3: Subscribe to market data channels.
    // The CFD feed's REST poll thread starts regardless of the active
    // direction — switching to CFD must be instant (no warm-up wait).
    // Each feed polls only its own market's symbols (per-market filtering).
    if (spot_feed)
    {
        spot_feed->start(spot_symbols);
    }
    if (futures_feed)
    {
        futures_feed->start(futures_symbols);
    }
    if (cfd_feed)
    {
        cfd_feed->start(cfd_symbols);
    }
    log->info("[L3] Market feed(s) started (spot={} futures={} cfd={} symbols)",
              spot_symbols.size(), futures_symbols.size(), cfd_symbols.size());

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

    // Position sync interval: 10 seconds / 200ms sleep = 50 iterations.
    // Hot-reconciles the engine position view with the exchange so manual
    // app-side closes (the user's normal habit) are picked up without a
    // restart, and ghosts get pruned (M21).
    constexpr int kPositionSyncIntervalTicks = 50;
    int position_sync_counter = 0;

    // Embedded REPL: enabled when stdin is a TTY. Commands dispatch
    // directly to EngineServices in-process (no socket round-trip).
    bool repl_enabled = stdinIsTty();
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
            // Wait on stdin with a 200ms timeout (EINTR-safe).
            const int rc = waitStdinReadable(200);
            if (rc > 0)
            {
                char buf[512];
                const std::ptrdiff_t n = readStdin(buf, sizeof(buf));
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

        // Maker-first sweep: cancel expired post-only attempts and re-issue
        // the remainder as market orders. Takes rest_mutex internally — the
        // loop body holds no locks at this point (logSystemHeartbeat locks
        // internally).
        order_flow.sweepMakerAttempts();
        // GridManager::tick takes wall-clock epoch ms (daily boundary, spike
        // windows, signal freshness all use it).
        grid_mgr.tick(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

        // Funding-window monitor (slow gate inside — polls at most every
        // poll_sec; object only exists when enabled and futures REST is up).
        if (funding_watch)
        {
            funding_watch->tick(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        }

        // M31: daily kline sync day-boundary check (µs — fetch runs on the
        // worker thread, never blocks the main loop).
        if (kline_sync)
        {
            kline_sync->tick(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        }

        // L7 gold trader: USD dynamic stop ladder for XAUUSD CFD. The
        // supervisor throttles itself to [gold_trader] poll_interval_ms (1 s
        // by default), so the 5 Hz main loop adds no measurable cost.
        //
        // Deliberately ticked BEFORE the periodic position sync: sync is a
        // synchronous REST call behind the shared mutex and can stall the
        // loop for seconds, which would otherwise push the 1-second peak
        // sample past its deadline.
        if (gold_trader)
        {
            (void)gold_trader->tick(std::chrono::system_clock::now());
        }

        // Periodic position sync (every ~10s): imports exchange-side
        // positions and prunes local ghosts. Takes rest_mutex internally.
        if (++position_sync_counter >= kPositionSyncIntervalTicks)
        {
            position_sync_counter = 0;
            (void)services.syncPositions();
        }

        // L7 protective exits: engine-side stop-loss / take-profit. Runs
        // every tick; each position is throttled to one close attempt per
        // kRetryCooldownSeconds inside the manager.
        (void)protective_exits.tick(std::chrono::system_clock::now());

        // Strategy Flat exits queued by the signal callback. Only positions
        // owned by the signalling strategy (or untagged ones) on that symbol
        // are closed — another strategy's book is not ours to touch.
        {
            std::vector<pulse::strategy::TradingSignal> flats;
            {
                std::lock_guard flat_lock(flat_mutex);
                flats.swap(pending_flats);
            }
            for (const auto &sig : flats)
            {
                for (const auto &pos : position_mgr.getPositionsBySymbol(sig.symbol))
                {
                    if (!pos.strategy_id.empty() && pos.strategy_id != sig.strategy_id)
                    {
                        continue;
                    }
                    nlohmann::json close_params;
                    close_params["position_id"] = pos.position_id;
                    const auto close_result = services.closePosition(close_params);
                    if (!pulse::ok(close_result))
                    {
                        log->warn("[L6] Flat exit failed for {}: {}",
                                  pos.position_id,
                                  pulse::error(close_result).message);
                    }
                }
            }
        }

        // L7 exchange-side protective stops (audit B1). One call covers both
        // entries filled while running and positions imported by
        // sync_positions after a restart (which never pass through the fill
        // path). The internal cache makes warm calls REST-free.
        if (cfg.risk.exchange_stop.enabled)
        {
            (void)services.ensureFuturesStops();
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
                &drawdownGuard,
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

    // M32 signal journal — drains + checkpoints (strategies are already
    // stopped above, so no signal can arrive after this point).
#ifdef PULSE_ENABLE_SQLITE
    if (signal_recorder)
    {
        signal_recorder->stop();
        log->info("[L8+] Signal journal stopped");
    }
#endif

    // L8+: Market data recorder — drains + checkpoints the WAL (must stop
    // AFTER the feeds so no events are lost, BEFORE the trade recorder).
#ifdef PULSE_ENABLE_SQLITE
    if (market_recorder)
    {
        market_recorder->stop();
        log->info("[L8+] Market data recorder stopped");
    }
#endif

    // L1: WebSockets
    if (futures_ws) { futures_ws->stop(); }
    if (spot_ws) { spot_ws->stop(); }
    log->info("[L1] WebSocket(s) disconnected");

    // Summary
    // M31: stop the daily kline sync worker (join) before the DB writers go
    // away — it may be mid-fetch with the sqlite reader open.
    if (kline_sync)
    {
        kline_sync.reset();
    }

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
    // An engine with [control] token set requires the handshake before any
    // other request; an empty token leaves the connection unauthenticated.
    client.setToken(cfg.control.token);
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
    client->setToken(mcp_cfg.control.token);
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
        if ("trade" == first || "cli" == first || "mcp" == first
            || "backtest" == first || "kline-store" == first)
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
    if ("backtest" == subcommand)
    {
        return pulse::runBacktest(argc, argv);
    }
    if ("kline-store" == subcommand)
    {
        return pulse::runKlineStore(argc, argv);
    }
    return runTrade(argc, argv);
}
