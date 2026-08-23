// backtest_engine.cpp — Top-level orchestration for one backtest run (M29)

#include "backtest/BacktestEngine.hpp"

#include "backtest/BacktestAccount.hpp"
#include "backtest/BacktestReport.hpp"
#include "backtest/GateKlineFetcher.hpp"
#include "backtest/KlineLoader.hpp"
#include "backtest/ReplayDriver.hpp"
#include "backtest/SqliteKlineReader.hpp"
#include "core/config_loader.hpp"
#include "exchange/GateRestClient.hpp"
#include "logging/Logger.hpp"
#include "strategy/StrategyRegistry.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>

namespace pulse::backtest
{

namespace
{

/// Default window when neither from nor to is given (7 days of 1m bars).
constexpr std::int64_t kDefaultWindowMs = 7LL * 24 * 3600 * 1000;

/// Seed options from a trading.toml instance (CLI-explicit values win).
/// Returns true when an instance matched (name + symbol).
bool seedFromConfig(BacktestOptions &opts, const PulseConfig &cfg)
{
    for (const auto &inst : cfg.strategy.strategies)
    {
        if (inst.name != opts.strategy_name || inst.symbol != opts.symbol)
        {
            continue;
        }
        if (opts.order_quantity <= 0.0)
        {
            opts.order_quantity = inst.order_quantity;
        }
        opts.min_confidence = inst.min_confidence;
        if (opts.quanto_multiplier <= 0.0)
        {
            opts.quanto_multiplier = 1.0;
        }
        return true;
    }
    return false;
}

/// Resolve an unspecified window end from local coverage; falls back to a
/// default lookback when there is no local data.
Result<std::pair<std::int64_t, std::int64_t>> resolveWindow(
    const BacktestOptions &opts, SqliteKlineReader *sqlite)
{
    std::int64_t from = opts.from_ms;
    std::int64_t to = opts.to_ms;

    if (0 == to)
    {
        if (sqlite)
        {
            auto cov = sqlite->coverage(opts.symbol, opts.market_type);
            if (ok(cov) && value(cov).has_value())
            {
                to = value(cov)->second;
            }
        }
        if (0 == to)
        {
            const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            to = now;
        }
    }
    if (0 == from)
    {
        if (sqlite)
        {
            auto cov = sqlite->coverage(opts.symbol, opts.market_type);
            if (ok(cov) && value(cov).has_value())
            {
                from = value(cov)->first;
            }
        }
        if (0 == from)
        {
            from = to - kDefaultWindowMs;
        }
    }

    if (from > to)
    {
        return PulseError{ ErrorCode::BacktestRangeInvalid,
            "Resolved window invalid: from " + std::to_string(from)
                + " > to " + std::to_string(to) };
    }
    return std::make_pair(from, to);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

BacktestEngine::BacktestEngine(BacktestOptions opts)
    : m_opts{ std::move(opts) }
{
}

// ---------------------------------------------------------------------------
// run
// ---------------------------------------------------------------------------

Result<std::string> BacktestEngine::run()
{
    // 1. Optional trading.toml instance seeding (CLI-explicit values win).
    if (!m_opts.config_path.empty())
    {
        auto cfg = loadConfigFile(m_opts.config_path);
        if (!ok(cfg))
        {
            return PulseError{ ErrorCode::BacktestConfigInvalid,
                "Failed to load config " + m_opts.config_path + ": "
                    + error(cfg).message };
        }
        if (!seedFromConfig(m_opts, value(cfg)))
        {
            PULSE_LOG_WARN("backtest", "No strategy instance named '{}' on "
                           "symbol '{}' in config — running with CLI defaults",
                           m_opts.strategy_name, m_opts.symbol);
        }
    }

    // No config and no --quantity: fall back to the strategy default size
    // (quantity is only a PnL scaling factor in backtest).
    if (m_opts.order_quantity <= 0.0)
    {
        m_opts.order_quantity = 0.001;
        PULSE_LOG_WARN("backtest", "No order_quantity (CLI/config) — using "
                       "default 0.001; pass --quantity or --config to size");
    }

    // Reject unknown strategy names explicitly (the registry fallback would
    // silently produce a passive instance with zero signals).
    auto registry = strategy::makeBuiltinStrategyRegistry();
    const auto registered = registry.registeredNames();
    if (std::find(registered.begin(), registered.end(), m_opts.strategy_name)
        == registered.end())
    {
        std::string known;
        for (const auto &name : registered)
        {
            known += (known.empty() ? "" : ", ") + name;
        }
        return PulseError{ ErrorCode::BacktestConfigInvalid,
            "Unknown strategy '" + m_opts.strategy_name
                + "' (registered: " + known + ")" };
    }

    // 2. Data sources. SqliteKlineReader is available even without the
    // SQLite build (its header only forward-declares SQLite::Database); the
    // reader is only CONSTRUCTED when SQLite is enabled.
    SqliteKlineReader *sqlite = nullptr;
#ifdef PULSE_ENABLE_SQLITE
    auto sqlite_reader = std::make_unique<SqliteKlineReader>(m_opts.sqlite_db_path);
    sqlite = sqlite_reader.get();
#endif

    ExchangeConfig rest_cfg;
    rest_cfg.restBaseUrl = "https://api.gateio.ws";
    exchange::GateRestClient rest(rest_cfg, m_opts.market_type);
    GateKlineFetcher api_fetcher(rest);

    // 3. Resolve the window, then load candles.
    auto window = resolveWindow(m_opts, sqlite);
    if (!ok(window))
    {
        return error(window);
    }
    m_opts.from_ms = value(window).first;
    m_opts.to_ms = value(window).second;

    KlineLoader loader(sqlite, api_fetcher);
    KlineLoadStats load_stats;
    KlineLoadRequest req;
    req.symbol = m_opts.symbol;
    req.market_type = m_opts.market_type;
    req.from_ms = m_opts.from_ms;
    req.to_ms = m_opts.to_ms;
    req.interval_ms = m_opts.interval_ms;
    req.api_backfill = m_opts.api_backfill;
    req.cache_writeback = m_opts.cache_writeback;

    auto candles = loader.load(req, load_stats);
    if (!ok(candles))
    {
        return error(candles);
    }

    // 4. Replay + fills (reuse the registry built in step 1).
    ReplayDriver driver(m_opts, registry);
    BacktestAccount account(m_opts);
    auto replay = driver.run(value(candles), account);
    if (!ok(replay))
    {
        return error(replay);
    }

    // 5. Window-end flattening, then report.
    account.closeAll(value(candles).back().open_time, value(candles).back().close);
    const auto stats = account.stats();

    const auto &rr = value(replay);
    BacktestReport report(m_opts, stats, load_stats, rr.signals,
                          rr.warmup_candles, rr.candles_fed);
    const std::string table = report.formatTable();

    if (!m_opts.json_export_path.empty())
    {
        std::ofstream out(m_opts.json_export_path);
        if (!out)
        {
            return PulseError{ ErrorCode::BacktestConfigInvalid,
                "Cannot write JSON export to " + m_opts.json_export_path };
        }
        out << report.toJson().dump(2);
        PULSE_LOG_INFO("backtest", "Report exported to {}", m_opts.json_export_path);
    }

    return table;
}

} // namespace pulse::backtest
