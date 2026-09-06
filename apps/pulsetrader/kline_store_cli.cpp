// kline_store_cli.cpp — `pulsetrader kline-store` subcommand (M33, A3)
//
// Local 1m-history accumulation for backtesting ANY Gate futures coin.
// Gate REST keeps only ~10000 recent 1m points (≈6.9 days), so a ~1-month
// backtest window needs a store that pulls the trailing window daily and
// merges it into kline_bars (idempotent). Outages ≤ ~6.9 days self-heal on
// the next run; a coin first stored today reaches ~30 days in ~30 days.
//
// Modes:
//   normal  — fetch the trailing window per symbol (REST, no API key) + merge
//   --import SOURCE — one-shot copy of existing futures rows from another
//             kline_bars database (e.g. the old data/trades.db) into --db

#include "kline_store_cli.hpp"

#include "backtest/GateKlineFetcher.hpp"
#include "backtest/KlineStore.hpp"
#include "backtest/QuantoResolver.hpp"
#include "backtest/SqliteKlineReader.hpp"
#include "core/config_loader.hpp"
#include "core/TimeUtil.hpp"
#include "exchange/GateRestClient.hpp"
#include "market/SymbolRegistry.hpp"

#include <chrono>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace pulse
{

namespace
{

constexpr std::int64_t kMinuteMs = 60'000;
constexpr std::int64_t kFuturesPoints = 9'995; ///< Gate REST depth (DailyKlineSync).
constexpr std::int64_t kSpotPoints = 9'990;

void printStoreUsage(const char *prog)
{
    std::cout
        << "pulseTrader kline-store — daily 1m kline accumulation (M33)\n\n"
        << "Gate REST keeps only ~10000 recent 1m candles (~6.9 days) per futures\n"
        << "symbol. This tool pulls the trailing window into the local kline_bars\n"
        << "database every run (idempotent) — run it daily (cron: 0 7,23 * * *)\n"
        << "and any symbol's history grows to ~1 month and beyond. No API key\n"
        << "needed (public REST).\n\n"
        << "Usage:\n"
        << "  " << prog << " kline-store [options]\n"
        << "  " << prog << " kline-store --import SOURCE_DB [options]\n\n"
        << "Options:\n"
        << "  --symbols LIST        comma-separated futures symbols, e.g. DOGE_USDT,PEPE_USDT\n"
        << "                        (default: [backtest].store_symbols, else the enabled\n"
        << "                        futures strategy instances' symbols from --config)\n"
        << "  --market TYPE         futures | spot (default futures)\n"
        << "  --db PATH             target kline_bars database (default data/trades.db;\n"
        << "                        overrides [backtest].store_db)\n"
        << "  --config PATH         trading.toml ([backtest] defaults + instance symbols)\n"
        << "  --import SOURCE_DB    copy existing futures rows from SOURCE_DB (any schema\n"
        << "                        version) into --db; --symbols filters the copy\n"
        << "  --contracts-cache P   contract cache path (default data/contracts_cache.json)\n"
        << "  --no-contract-cache   always fetch the contract list for symbol validation\n"
        << "  --help                this message\n";
}

/// Split "A,B,C" into tokens (trims spaces). Empty input → empty vector.
std::vector<std::string> splitSymbols(const std::string &list)
{
    std::vector<std::string> out;
    std::stringstream ss(list);
    std::string item;
    while (std::getline(ss, item, ','))
    {
        const auto first = item.find_first_not_of(' ');
        const auto last = item.find_last_not_of(' ');
        if (std::string::npos != first)
        {
            out.push_back(item.substr(first, last - first + 1));
        }
    }
    return out;
}

/// Enabled futures strategy instances' symbols (mirrors main.cpp symbols_for).
std::vector<std::string> futuresInstanceSymbols(const PulseConfig &cfg)
{
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const auto &inst : cfg.strategy.strategies)
    {
        if (!inst.enabled || MarketType::Futures != inst.market_type)
        {
            continue;
        }
        if (seen.insert(inst.symbol).second)
        {
            out.push_back(inst.symbol);
        }
    }
    return out;
}

std::string isoUtc(std::int64_t epoch_ms)
{
    return formatEpochMs(epoch_ms, DisplayTimezone::utc());
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// runKlineStore
// ---------------------------------------------------------------------------

int runKlineStore(int argc, char *argv[])
{
    std::vector<std::string> symbols_cli;
    MarketType market_type = MarketType::Futures;
    std::string db_path;
    std::string config_path;
    std::string import_source;
    std::string contract_cache = "data/contracts_cache.json";
    bool use_contract_cache = true;

    for (int i = 0; i < argc; ++i)
    {
        const std::string arg = argv[i];
        const auto next = [&]() -> std::string
        {
            return (i + 1 < argc) ? std::string{ argv[++i] } : std::string{};
        };

        if ("--symbols" == arg)
        {
            const auto parts = splitSymbols(next());
            symbols_cli.insert(symbols_cli.end(), parts.begin(), parts.end());
        }
        else if ("--market" == arg)
        {
            const std::string m = next();
            if ("spot" == m)
            {
                market_type = MarketType::Spot;
            }
            else if ("futures" == m)
            {
                market_type = MarketType::Futures;
            }
            else
            {
                std::cerr << "Unknown market: " << m
                          << " (kline-store supports spot|futures; CFD has no "
                             "REST time-range endpoint)\n";
                return 2;
            }
        }
        else if ("--db" == arg)
        {
            db_path = next();
        }
        else if ("--config" == arg)
        {
            config_path = next();
        }
        else if ("--import" == arg)
        {
            import_source = next();
        }
        else if ("--contracts-cache" == arg)
        {
            contract_cache = next();
        }
        else if ("--no-contract-cache" == arg)
        {
            use_contract_cache = false;
        }
        else if ("--help" == arg || "-h" == arg)
        {
            printStoreUsage(argv[0]);
            return 0;
        }
        else
        {
            std::cerr << "Unknown kline-store argument: " << arg << "\n";
            printStoreUsage(argv[0]);
            return 2;
        }
    }

    // --- Config defaults ([backtest] section) ---
    PulseConfig cfg;
    if (!config_path.empty())
    {
        auto loaded = loadConfigFile(config_path);
        if (!ok(loaded))
        {
            std::cerr << "Failed to load config " << config_path << ": "
                      << error(loaded).message << "\n";
            return 1;
        }
        cfg = value(loaded);
        if (db_path.empty())
        {
            db_path = cfg.backtest.store_db;
        }
        if (use_contract_cache)
        {
            contract_cache = cfg.backtest.contract_cache_path;
        }
    }
    if (db_path.empty())
    {
        db_path = cfg.backtest.store_db;
    }

    // Symbols: CLI union > config store_symbols > futures strategy instances.
    std::vector<std::string> symbols = symbols_cli;
    if (symbols.empty())
    {
        symbols = cfg.backtest.store_symbols;
    }
    if (symbols.empty())
    {
        symbols = futuresInstanceSymbols(cfg);
    }
    if (symbols.empty())
    {
        std::cerr << "No symbols to store: pass --symbols LIST, add "
                     "[backtest].store_symbols, or point --config at a "
                     "trading.toml with futures strategy instances\n";
        return 2;
    }

    // --- Import mode: one-shot copy from an existing kline_bars database ---
    if (!import_source.empty())
    {
#ifdef PULSE_ENABLE_SQLITE
        auto sink = std::make_unique<backtest::SqliteKlineReader>(db_path);
        auto imported = backtest::importFuturesFromDb(import_source, symbols, *sink);
        if (!ok(imported))
        {
            std::cerr << "Import failed: " << error(imported).message << "\n";
            return 1;
        }
        std::cout << "Imported " << value(imported) << " new futures rows from "
                  << import_source << " into " << db_path;
        if (!symbols_cli.empty() || !cfg.backtest.store_symbols.empty())
        {
            std::cout << " (filtered to " << symbols.size() << " symbols)";
        }
        std::cout << "\n";
        return 0;
#else
        std::cerr << "kline-store --import requires a SQLite build "
                     "(-DPULSE_ENABLE_SQLITE=ON)\n";
        return 1;
#endif
    }

    // --- Normal mode: fetch the trailing window per symbol + merge ---
#ifdef PULSE_ENABLE_SQLITE
    ExchangeConfig rest_cfg;
    rest_cfg.restBaseUrl = "https://api.gateio.ws";
    exchange::GateRestClient rest(rest_cfg, market_type);
    backtest::GateKlineFetcher api(rest);
    auto sink = std::make_unique<backtest::SqliteKlineReader>(db_path);

    // Validate futures symbols against the contract list before the ~30 REST
    // requests per symbol (typo'd symbols fail fast here).
    backtest::QuantoResolver resolver(rest, use_contract_cache ? contract_cache
                                                               : std::string{});
    if (MarketType::Futures == market_type)
    {
        for (const auto &symbol : symbols)
        {
            auto meta = resolver.futuresContract(symbol);
            if (!ok(meta))
            {
                std::cerr << "kline-store aborted: "
                          << error(meta).message << "\n";
                return 1;
            }
        }
    }

    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const std::int64_t depth_points = (MarketType::Spot == market_type)
        ? kSpotPoints : kFuturesPoints;
    const std::int64_t from_ms = now_ms - depth_points * kMinuteMs;
    const std::int64_t to_ms = now_ms - kMinuteMs; // drop the forming minute

    backtest::KlineStore store(api, *sink);
    std::cout << "Storing trailing 1m candles into " << db_path << "\n";
    int failures = 0;
    for (const auto &symbol : symbols)
    {
        auto stats = store.storeWindow(symbol, market_type, from_ms, to_ms, now_ms);
        if (!ok(stats))
        {
            std::cerr << "[" << symbol << "] " << error(stats).message << "\n";
            ++failures;
            continue;
        }
        const auto &s = value(stats);
        std::cout << "[" << symbol << "] fetched " << s.rows_fetched
                  << " (+" << s.rows_new << " new)";
        if (0 != s.rows_new)
        {
            std::cout << ", coverage " << isoUtc(s.first_open_ms) << " .. "
                      << isoUtc(s.last_open_ms);
        }
        std::cout << "\n";
    }

    std::cout << "\nDone (" << (symbols.size() - static_cast<std::size_t>(failures))
              << "/" << symbols.size() << " symbols ok). Data model: REST keeps "
                 "~6.9 days; storage accumulates backward — a new coin reaches "
                 "a ~30-day window after ~30 days of daily runs. Run twice "
                 "daily (cron 0 7,23) to survive laptop sleep (outages <= ~6.9 "
                 "days self-heal).\n";
    return (0 == failures) ? 0 : 1;
#else
    std::cerr << "kline-store requires a SQLite build (-DPULSE_ENABLE_SQLITE=ON)\n";
    return 1;
#endif
}

} // namespace pulse
