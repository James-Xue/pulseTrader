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

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
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

    // L1: Exchange — credentials from environment
    cfg.exchange.apiKey     = env_or("GATE_API_KEY", "");
    cfg.exchange.apiSecret  = env_or("GATE_API_SECRET", "");
    cfg.exchange.proxyUrl   = env_or("HTTPS_PROXY", env_or("HTTP_PROXY", ""));
    cfg.exchange.restBaseUrl = "https://api.gateio.ws";
    cfg.exchange.wsUrl       = "wss://api.gateio.ws/ws/v4/";

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

// ===========================================================================
// main
// ===========================================================================
int main(int argc, char* argv[])
{
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
            std::cerr << "  source .env  or  export GATE_API_KEY=... GATE_API_SECRET=...\n";
        }
        else
        {
            std::cerr << "  Use from_env:GATE_API_KEY / from_env:GATE_API_SECRET in TOML.\n";
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
    if (!cfg.exchange.proxyUrl.empty())
    {
        log->info("Proxy:    {}", cfg.exchange.proxyUrl);
    }

    // ------------------------------------------------------------------
    // 3. L1: Exchange clients
    // ------------------------------------------------------------------
    pulse::exchange::GateRestClient rest_client(cfg.exchange);
    pulse::exchange::GateWsClient   ws_client(cfg.exchange);

    log->info("[L1] Exchange clients created");

    // ------------------------------------------------------------------
    // 4. L3: Market Data
    // ------------------------------------------------------------------
    pulse::market::MarketFeed market_feed(ws_client, rest_client);

    log->info("[L3] Market feed created");

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
    // 6. L8: Order Execution
    // ------------------------------------------------------------------
    pulse::execution::OrderExecutor executor(rest_client);
    pulse::execution::OrderTracker  order_tracker(ws_client, rest_client);

    log->info("[L8] Order executor + tracker created");

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

        pulse::strategy::StrategyContext ctx(market_feed, risk_mgr,
                                             executor, inst_cfg);

        auto strat = create_strategy(inst_cfg.name, ctx);
        if (!strat)
        {
            log->warn("Unknown strategy '{}', skipping", inst_cfg.name);
            continue;
        }

        log->info("[L6] Registered strategy: {} on {} (qty={}, conf={:.2f})",
                  inst_cfg.name, inst_cfg.symbol,
                  inst_cfg.order_quantity, inst_cfg.min_confidence);

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

    // We need a StrategyParams& for the heartbeat.  Since each strategy
    // owns its own params, we create a shared one here and the heartbeat
    // writes AI deltas to it.  Strategies read their own params on the
    // hot path.  (Future: unify all strategies to share one params.)
    pulse::strategy::StrategyParams shared_params;

    std::unique_ptr<pulse::heartbeat::HeartbeatScheduler> heartbeat;
    if (cfg.ai.heartbeatIntervalSec > 0 && !cfg.ai.apiKey.empty())
    {
        heartbeat = std::make_unique<pulse::heartbeat::HeartbeatScheduler>(
            cfg.ai, ai_pipeline, shared_params);
        log->info("[L5] Heartbeat scheduler created (interval: {}s)",
                  cfg.ai.heartbeatIntervalSec);
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
        dashboard_state = std::make_unique<pulse::webui::DashboardState>(
            cfg.webui, market_feed, strategy_mgr, risk_mgr,
            order_tracker, ai_pipeline);

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
            req.quantity = cfg.strategy.strategies.empty()
                           ? 0.001
                           : cfg.strategy.strategies[0].order_quantity;
            req.price    = sig.price;

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
            auto result = executor.place_order(req);
            if (!ok(result))
            {
                auto err = error(result);
                log_app->error("Order FAILED: {} (code={})",
                               err.message, static_cast<int>(err.code));
                return;
            }

            auto& resp = value(result);
            log_app->info("Order PLACED: id={} status={}",
                          resp.order_id,
                          static_cast<int>(resp.status));

            // Track order lifecycle via WS + REST polling fallback.
            order_tracker.track_order(resp.order_id, req.symbol,
                                      req.side, req.type,
                                      req.quantity, sig.price);
        });

    // ------------------------------------------------------------------
    // 11. Wire: order completion → update position manager + log
    // ------------------------------------------------------------------
    order_tracker.set_completion_callback(
        [&](const pulse::execution::ExecutionReport& report)
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

            // Update position manager.
            if (pulse::Side::Buy == report.side)
            {
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
                // For sells, try to close matching positions.
                auto positions = position_mgr.get_positions_by_symbol(
                    report.symbol);
                for (const auto& pos : positions)
                {
                    if (position_mgr.close_position(
                            pos.position_id, report.filled_qty,
                            report.avg_fill_price))
                    {
                        log_app->info("Closed position {}", pos.position_id);
                    }
                    else
                    {
                        log_app->warn("Failed to close position {}",
                                      pos.position_id);
                    }
                }
            }

            // Update drawdown guard with realized PnL.
            double pnl = 0.0;  // Simplified — real PnL needs position tracking.
            drawdown_guard.record_pnl(pnl);
        });

    // ------------------------------------------------------------------
    // 12. Signal handler — SIGINT / SIGTERM
    // ------------------------------------------------------------------
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    // ------------------------------------------------------------------
    // 13. Start all layers
    // ------------------------------------------------------------------
    log->info("Starting trading engine...");

    // L1: WebSocket connection.
    ws_client.start();
    log->info("[L1] WebSocket connecting...");

    // L3: Subscribe to market data channels.
    market_feed.start(cfg.symbols);
    log->info("[L3] Market feed started for {} symbol(s)", cfg.symbols.size());

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
    // 14. Main loop — wait for stop signal
    // ------------------------------------------------------------------
    log->info("Trading engine started. Press Ctrl+C to stop.");
    log->info("──────────────────────────────────────────────────");

    while (!g_stop_requested.load(std::memory_order_acquire))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
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

    // L3: Market feed
    market_feed.stop();
    log->info("[L3] Market feed stopped");

    // L1: WebSocket
    ws_client.stop();
    log->info("[L1] WebSocket disconnected");

    // Summary
    auto portfolio = position_mgr.portfolio_summary();
    log->info("Final portfolio: {} open position(s), notional {:.2f} USDT",
              position_mgr.open_position_count(), portfolio.total_notional);

    log->info("pulseTrader stopped. Goodbye.");

    // L2: Logger (must be last)
    pulse::logging::Logger::shutdown();

    return 0;
}
