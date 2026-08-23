#pragma once
// backtest_cli.hpp — `pulsetrader backtest` subcommand entry point (M29)

namespace pulse
{

/// Parse backtest CLI args and run the engine. Returns the process exit
/// code (0 = success, non-zero on any error path).
int runBacktest(int argc, char *argv[]);

} // namespace pulse
