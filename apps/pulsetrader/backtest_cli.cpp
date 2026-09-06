// backtest_cli.cpp — `pulsetrader backtest` subcommand (M29)
//
// Parses CLI options into BacktestOptions, runs BacktestEngine, prints the
// report table to stdout and returns a process exit code. Errors exit
// non-zero after printing the message to stderr.

#include "backtest_cli.hpp"

#include "backtest/BacktestEngine.hpp"
#include "backtest/backtest_types.hpp"
#include "core/PulseError.hpp"
#include "core/TimeUtil.hpp"
#include "core/config.hpp"
#include "core/types.hpp"

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
        << "  --quanto Q            futures contract size (default: auto from Gate\n"
        << "                        contract list, cached 12h; spot = 1.0)\n"
        << "  --contracts-cache P   contract list cache path (default data/contracts_cache.json)\n"
        << "  --no-contract-cache   always fetch the contract list, never read the cache\n"
        << "  --fee-rate R          taker fee; <0 none, 0 market default (futures 0.0005)\n"
        << "  --cooldown SEC        replay cooldown (default 0 = disabled)\n"
        << "  --close-mode MODE     flip | independent (default flip)\n"
        << "  --param KEY=VALUE     strategy param override (repeatable): atomic keys\n"
        << "                        (order_quantity, min_confidence, ema_fast_period,\n"
        << "                        ema_slow_period, bb_*, supertrend_*, cooldown_seconds,\n"
        << "                        stop_loss_pct, take_profit_pct) set the hot params;\n"
        << "                        ANY other key routes to custom_params (eth_*,\n"
        << "                        res_ema_p1..p5, ...). CLI wins over --config values.\n"
        << "  --news TIME[/X[/Y]]   preset UTC news window (repeatable): entries blocked\n"
        << "                        [T-X, T+Y); a position opened before T is flattened\n"
        << "                        from T-X (major-news gate, iron-trader rules §4.6).\n"
        << "                        X = close_before_min, Y = resume_after_min, default\n"
        << "                        15/15; e.g. --news 2026-09-16T18:00:00Z/15/15\n"
        << "  --no-api              disable Gate API gap fill (local data only)\n"
        << "  --no-cache            do not write API-fetched candles back to sqlite\n"
        << "  --config PATH         trading.toml for instance params (quantity/confidence/\n"
        << "                        custom_params)\n"
        << "  --db PATH             kline_bars database (default data/trades.db)\n"
        << "  --json PATH           export full JSON report to PATH\n"
        << "  --help                this message\n";
}

/// Parse a time argument: bare epoch (<=10 digits = seconds, else ms), or
/// ISO UTC "YYYY-MM-DD" / "YYYY-MM-DDTHH:MM:SS". Returns 0 on parse error
/// (callers distinguish 0 = unset). Shared logic: core::parseEpochMsText.
std::int64_t parseTimeArg(const std::string &text, bool *ok_flag)
{
    std::int64_t parsed = 0;
    *ok_flag = parseEpochMsText(text, parsed);
    return *ok_flag ? parsed : 0;
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
        else if ("--no-contract-cache" == arg)
        {
            opts.contract_cache_path.clear(); // always fetch the contract list
        }
        else if ("--contracts-cache" == arg)
        {
            opts.contract_cache_path = next();
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
        else if ("--param" == arg)
        {
            const std::string kv = next();
            const auto eq = kv.find('=');
            if (std::string::npos == eq || 0 == eq || kv.size() - 1 == eq)
            {
                std::cerr << "--param expects KEY=VALUE (e.g. --param ema_fast_period=9)\n";
                return 2;
            }
            opts.param_overrides[kv.substr(0, eq)] = std::stod(kv.substr(eq + 1));
        }
        else if ("--news" == arg)
        {
            // Grammar: TIME[/close_before_min[/resume_after_min]] — repeatable,
            // one window per occurrence. Fractional minutes allowed.
            const std::string spec = next();
            if (spec.empty())
            {
                std::cerr << "--news expects TIME[/X[/Y]] "
                             "(e.g. --news 2026-09-16T18:00:00Z/15/15)\n";
                return 2;
            }
            NewsWindow win;
            const std::size_t slash = spec.find('/');
            const std::string time_text = spec.substr(0, slash);
            bool ok_flag = false;
            win.event_open_ms = parseTimeArg(time_text, &ok_flag);
            if (!ok_flag)
            {
                std::cerr << "Could not parse --news time: " << time_text << "\n";
                return 2;
            }
            if (std::string::npos != slash)
            {
                const std::size_t slash2 = spec.find('/', slash + 1);
                const std::string x_text = spec.substr(slash + 1, slash2 - slash - 1);
                const std::string y_text = (std::string::npos != slash2)
                    ? spec.substr(slash2 + 1) : std::string{};
                if (!x_text.empty())
                {
                    win.close_before_min = std::stod(x_text);
                }
                if (!y_text.empty())
                {
                    win.resume_after_min = std::stod(y_text);
                }
            }
            if (win.close_before_min < 0.0 || win.resume_after_min < 0.0)
            {
                std::cerr << "--news X/Y must be >= 0 (got: " << spec << ")\n";
                return 2;
            }
            opts.news_windows.push_back(win);
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

    // quanto <= 0 means "unset": BacktestEngine auto-resolves futures
    // contract sizes from the public Gate contract list (cached), spot = 1.0.
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
