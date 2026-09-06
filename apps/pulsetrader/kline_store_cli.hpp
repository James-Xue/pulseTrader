#pragma once
// kline_store_cli.hpp — `pulsetrader kline-store` subcommand entry (M33)

namespace pulse
{

/// Parse kline-store CLI args, accumulate trailing klines into the local
/// sqlite store (or import from an existing kline_bars database), and print
/// per-symbol coverage. Returns the process exit code.
int runKlineStore(int argc, char *argv[]);

} // namespace pulse
