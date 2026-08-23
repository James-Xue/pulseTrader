// backtest_cli.cpp — `pulsetrader backtest` subcommand (M29)
//
// Parses CLI options into BacktestOptions, runs BacktestEngine, prints the
// report table to stdout and returns a process exit code. Errors exit
// non-zero after printing the message to stderr.

#include "backtest_cli.hpp"

#include "backtest/BacktestEngine.hpp"
#include "backtest/backtest_types.hpp"
#include "core/PulseError.hpp"
#include "core/types.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace pulse
{

namespace
{

void printBacktestUsage(const char *prog)
{
    std::cout
        << "pulseTrader backtest — offline strategy replay (M29)\n\n"
        << "Usage:\n"
        << "  " << prog << " backtest --strategy NAME --symbol PAIR [options]\n\n"
        << "Required:\n"
        << "  --strategy NAME       Registry key (ema_resonance_scalper, momentum_scalper,\n"
        << "                        mean_reversion_scalper, supertrend_scalper, eth_scalper)\n"
        << "  --symbol PAIR         e.g. ETH_USDT\n\n"
        << "Options:\n"
        << "  --market TYPE         spot | futures (default futures)\n"
        << "  --from TIME           epoch sec/ms or ISO UTC 'YYYY-MM-DD[THH:MM:SS]' (default auto)\n"
        << "  --to TIME             same formats (default auto)\n"
        << "  --interval MS         bar size in ms (default 60000; API fill is 1m only)\n"
        << "  --quantity QTY        order size (contracts for futures); 0 = trading.toml value\n"
        << "  --min-confidence C    confidence gate (default 0.6)\n"
        << "  --leverage L          display only, does not affect PnL (default 1)\n"
        << "  --quanto Q            futures contract size; defaults ETH_USDT=0.01 BTC_USDT=0.0001\n"
        << "  --fee-rate R          taker fee; <0 none, 0 market default (futures 0.0005)\n"
        << "  --cooldown SEC        replay cooldown (default 0 = disabled)\n"
        << "  --close-mode MODE     flip | independent (default flip)\n"
        << "  --no-api              disable Gate API gap fill (local data only)\n"
        << "  --no-cache            do not write API-fetched candles back to sqlite\n"
        << "  --config PATH         trading.toml for instance params (quantity/confidence)\n"
        << "  --db PATH             kline_bars database (default data/trades.db)\n"
        << "  --json PATH           export full JSON report to PATH\n"
        << "  --help                this message\n";
}

/// Parse a time argument: bare epoch (<=10 digits = seconds, else ms), or
/// ISO UTC "YYYY-MM-DD" / "YYYY-MM-DDTHH:MM:SS". Returns 0 on parse error
/// (callers distinguish 0 = unset).
std::int64_t parseTimeArg(const std::string &text, bool *ok_flag)
{
    // Pure numeric → epoch (seconds if < 1e12, else ms).
    bool all_digits = !text.empty();
    for (const char c : text)
    {
        all_digits = all_digits && (c >= '0' && c <= '9');
    }
    if (all_digits)
    {
        const std::int64_t v = std::stoll(text);
        *ok_flag = true;
        return (v < 1'000'000'000'000LL) ? v * 1000 : v;
    }

    // ISO UTC. Accept "YYYY-MM-DD" (midnight) and "YYYY-MM-DDTHH:MM:SS".
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (6 > std::sscanf(text.c_str(), "%d-%d-%dT%d:%d:%d",
                        &year, &month, &day, &hour, &minute, &second)
        && 3 > std::sscanf(text.c_str(), "%d-%d-%d", &year, &month, &day))
    {
        *ok_flag = false;
        return 0;
    }

    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    tm.tm_isdst = 0;

    const std::time_t secs = timegm(&tm);
    if (-1 == secs)
    {
        *ok_flag = false;
        return 0;
    }
    *ok_flag = true;
    return static_cast<std::int64_t>(secs) * 1000;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// runBacktest
// ---------------------------------------------------------------------------

int runBacktest(int argc, char *argv[])
{
    backtest::BacktestOptions opts;

    for (int i = 0; i < argc; ++i)
    {
        const std::string arg = argv[i];
        const auto next = [&]() -> std::string
        {
            return (i + 1 < argc) ? std::string{ argv[++i] } : std::string{};
        };

        if ("--strategy" == arg)
        {
            opts.strategy_name = next();
        }
        else if ("--symbol" == arg)
        {
            opts.symbol = next();
        }
        else if ("--market" == arg)
        {
            const std::string m = next();
            if ("spot" == m)
            {
                opts.market_type = MarketType::Spot;
            }
            else if ("futures" == m || "contract" == m)
            {
                opts.market_type = MarketType::Futures;
            }
            else if ("cfd" == m)
            {
                opts.market_type = MarketType::Cfd;
            }
            else
            {
                std::cerr << "Unknown market: " << m << " (spot|futures|cfd)\n";
                return 2;
            }
        }
        else if ("--from" == arg)
        {
            bool ok_flag = false;
            const std::string value = next();
            opts.from_ms = parseTimeArg(value, &ok_flag);
            if (!ok_flag)
            {
                std::cerr << "Could not parse --from value: " << value << "\n";
                return 2;
            }
        }
        else if ("--to" == arg)
        {
            bool ok_flag = false;
            const std::string value = next();
            opts.to_ms = parseTimeArg(value, &ok_flag);
            if (!ok_flag)
            {
                std::cerr << "Could not parse --to value: " << value << "\n";
                return 2;
            }
        }
        else if ("--interval" == arg)
        {
            opts.interval_ms = std::stoll(next());
        }
        else if ("--quantity" == arg)
        {
            opts.order_quantity = std::stod(next());
        }
        else if ("--min-confidence" == arg)
        {
            opts.min_confidence = std::stod(next());
        }
        else if ("--leverage" == arg)
        {
            opts.leverage = std::stod(next());
        }
        else if ("--quanto" == arg)
        {
            opts.quanto_multiplier = std::stod(next());
        }
        else if ("--fee-rate" == arg)
        {
            opts.taker_fee_rate = std::stod(next());
        }
        else if ("--cooldown" == arg)
        {
            opts.cooldown_seconds = std::stod(next());
        }
        else if ("--close-mode" == arg)
        {
            const std::string mode = next();
            if ("independent" == mode)
            {
                opts.close_mode = backtest::CloseMode::Independent;
            }
            else if ("flip" == mode)
            {
                opts.close_mode = backtest::CloseMode::Flip;
            }
            else
            {
                std::cerr << "Unknown close mode: " << mode << " (flip|independent)\n";
                return 2;
            }
        }
        else if ("--no-api" == arg)
        {
            opts.api_backfill = false;
        }
        else if ("--no-cache" == arg)
        {
            opts.cache_writeback = false;
        }
        else if ("--config" == arg)
        {
            opts.config_path = next();
        }
        else if ("--db" == arg)
        {
            opts.sqlite_db_path = next();
        }
        else if ("--json" == arg)
        {
            opts.json_export_path = next();
        }
        else if ("--help" == arg || "-h" == arg)
        {
            printBacktestUsage(argv[0]);
            return 0;
        }
        else
        {
            std::cerr << "Unknown backtest argument: " << arg << "\n";
            printBacktestUsage(argv[0]);
            return 2;
        }
    }

    if (opts.strategy_name.empty() || opts.symbol.empty())
    {
        std::cerr << "backtest requires --strategy and --symbol\n";
        printBacktestUsage(argv[0]);
        return 2;
    }

    // Default futures quanto from the known contract sizes.
    if (opts.quanto_multiplier <= 0.0)
    {
        if ("ETH_USDT" == opts.symbol)
        {
            opts.quanto_multiplier = 0.01;
        }
        else if ("BTC_USDT" == opts.symbol)
        {
            opts.quanto_multiplier = 0.0001;
        }
        else
        {
            opts.quanto_multiplier = 1.0;
        }
    }

    backtest::BacktestEngine engine(opts);
    auto report = engine.run();
    if (!ok(report))
    {
        std::cerr << "Backtest failed: " << error(report).message << "\n";
        return 1;
    }
    std::cout << value(report);
    return 0;
}

} // namespace pulse
