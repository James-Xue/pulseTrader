// backtest_data_check.cpp — Manual smoke tool for the backtest engine (M29).
//
// Prints the kline_bars coverage and gap list for a symbol + market from the
// local SQLite database, optionally fetches one historical window from the
// Gate API to verify the new candlestick endpoints end-to-end, and optionally
// replays the local candles through a strategy (signal count + warmup check).
//
// Usage:
//   ./backtest_data_check [--db PATH] --symbol ETH_USDT [--market futures]
//                         [--api-fetch FROM_SEC TO_SEC]
//                         [--replay STRATEGY_NAME]
//
// Not part of CTest — manual verification only.

#include "backtest/BacktestAccount.hpp"
#include "backtest/GateKlineFetcher.hpp"
#include "backtest/KlineSource.hpp"
#include "backtest/ReplayDriver.hpp"
#include "core/PulseError.hpp"
#include "core/config.hpp"
#include "core/types.hpp"
#include "exchange/GateRestClient.hpp"
#include "strategy/StrategyRegistry.hpp"

#include <iostream>
#include <string>
#include <utility>

#ifdef PULSE_ENABLE_SQLITE
#include "backtest/SqliteKlineReader.hpp"
#endif

namespace
{

void printUsage()
{
    std::cout
        << "Usage: backtest_data_check [--db PATH] --symbol SYM [--market "
           "spot|futures]\n"
        << "                          [--api-fetch FROM_SEC TO_SEC]\n"
        << "                          [--replay STRATEGY_NAME]\n";
}

} // anonymous namespace

int main(int argc, char **argv)
{
    std::string db_path = "data/trades.db";
    std::string symbol = "ETH_USDT";
    std::string market = "futures";
    std::string replay_strategy;
    std::int64_t fetch_from = 0;
    std::int64_t fetch_to = 0;
    bool do_api_fetch = false;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if ("--db" == arg && i + 1 < argc)
        {
            db_path = argv[++i];
        }
        else if ("--symbol" == arg && i + 1 < argc)
        {
            symbol = argv[++i];
        }
        else if ("--market" == arg && i + 1 < argc)
        {
            market = argv[++i];
        }
        else if ("--replay" == arg && i + 1 < argc)
        {
            replay_strategy = argv[++i];
        }
        else if ("--api-fetch" == arg && i + 2 < argc)
        {
            fetch_from = std::stoll(argv[++i]);
            fetch_to = std::stoll(argv[++i]);
            do_api_fetch = true;
        }
        else if ("--help" == arg || "-h" == arg)
        {
            printUsage();
            return 0;
        }
        else
        {
            std::cerr << "Unknown argument: " << arg << "\n";
            printUsage();
            return 1;
        }
    }

    const pulse::MarketType mt = ("spot" == market)
        ? pulse::MarketType::Spot
        : ("cfd" == market) ? pulse::MarketType::Cfd : pulse::MarketType::Futures;

    std::cout << "Local coverage for " << symbol << " (" << market << ") in "
              << db_path << ":\n";

#ifdef PULSE_ENABLE_SQLITE
    pulse::backtest::SqliteKlineReader reader(db_path);
    auto cov = reader.coverage(symbol, mt);
    if (pulse::ok(cov) && pulse::value(cov).has_value())
    {
        std::cout << "  min open_time = " << pulse::value(cov)->first << " ("
                  << pulse::value(cov)->first / 1000 << "s)\n";
        std::cout << "  max open_time = " << pulse::value(cov)->second << " ("
                  << pulse::value(cov)->second / 1000 << "s)\n";

        // Count candles and list gaps (1m interval).
        auto rows = reader.fetch(symbol, mt, pulse::value(cov)->first,
                                 pulse::value(cov)->second);
        if (pulse::ok(rows))
        {
            std::cout << "  rows = " << pulse::value(rows).size() << "\n";
            const auto gaps = pulse::backtest::findKlineGaps(
                pulse::value(rows), pulse::value(cov)->first,
                pulse::value(cov)->second, 60'000);
            std::cout << "  gaps = " << gaps.size() << "\n";
            for (const auto &[g_from, g_to] : gaps)
            {
                std::cout << "    [" << g_from / 1000 << "s .. " << g_to / 1000
                          << "s]\n";
            }
        }
    }
    else
    {
        std::cout << "  (no coverage — table empty or database missing)\n";
    }
#else
    std::cout << "  (built without PULSE_ENABLE_SQLITE)\n";
#endif

    if (do_api_fetch)
    {
        std::cout << "API fetch [" << fetch_from << "s .. " << fetch_to
                  << "s]:\n";
        pulse::ExchangeConfig cfg;
        cfg.restBaseUrl = "https://api.gateio.ws";
        pulse::exchange::GateRestClient rest(cfg, mt);
        pulse::backtest::GateKlineFetcher fetcher(rest);
        auto result = fetcher.fetch(symbol, mt, fetch_from * 1000, fetch_to * 1000);
        if (ok(result))
        {
            std::cout << "  fetched " << value(result).size() << " candles\n";
            for (const auto &k : value(result))
            {
                std::cout << "    " << k.open_time / 1000 << "s  O=" << k.open
                          << " H=" << k.high << " L=" << k.low
                          << " C=" << k.close << " V=" << k.volume << "\n";
            }
        }
        else
        {
            std::cerr << "  fetch failed: " << error(result).message << "\n";
            return 1;
        }
    }

    if (!replay_strategy.empty())
    {
        std::cout << "Replay " << replay_strategy << " on local " << symbol
                  << " (" << market << "):\n";

#ifdef PULSE_ENABLE_SQLITE
        pulse::backtest::SqliteKlineReader reader(db_path);
        auto cov = reader.coverage(symbol, mt);
        if (!(pulse::ok(cov) && pulse::value(cov).has_value()))
        {
            std::cerr << "  no local coverage — nothing to replay\n";
            return 1;
        }
        auto rows = reader.fetch(symbol, mt, pulse::value(cov)->first,
                                 pulse::value(cov)->second);
        if (!pulse::ok(rows) || pulse::value(rows).empty())
        {
            std::cerr << "  no candles to replay\n";
            return 1;
        }
#else
        std::cerr << "  built without PULSE_ENABLE_SQLITE\n";
        return 1;
#endif

        pulse::backtest::BacktestOptions opts;
        opts.strategy_name = replay_strategy;
        opts.symbol = symbol;
        opts.market_type = mt;
        opts.order_quantity = 20.0;
        opts.quanto_multiplier = (pulse::MarketType::Futures == mt) ? 0.01 : 1.0;
        opts.min_confidence = 0.0;

        auto registry = pulse::strategy::makeBuiltinStrategyRegistry();
        pulse::backtest::ReplayDriver driver(opts, registry);
        pulse::backtest::BacktestAccount account(opts);
        auto result = driver.run(pulse::value(rows), account);
        if (!pulse::ok(result))
        {
            std::cerr << "  replay failed: " << pulse::error(result).message << "\n";
            return 1;
        }

        const auto &rr = pulse::value(result);
        std::cout << "  candles_fed = " << rr.candles_fed
                  << ", warmup = " << rr.warmup_candles
                  << ", signals = " << rr.signals.size() << "\n";
        for (const auto &s : rr.signals)
        {
            std::cout << "    " << s.candle_open_ms / 1000 << "s  "
                      << (pulse::strategy::SignalType::Buy == s.type ? "BUY  " : "SELL ")
                      << " conf=" << s.confidence << " price=" << s.price
                      << "  " << s.reason << "\n";
        }
        if (0 < rr.signals.size())
        {
            // Window-end flattening (the engine does this after the replay).
            const double last_close = pulse::value(rows).back().close;
            account.closeAll(pulse::value(rows).back().open_time, last_close);
            std::cout << "  trades = " << account.trades().size()
                      << ", net_pnl = " << account.realizedCash() << "\n";
        }
    }

    return 0;
}
