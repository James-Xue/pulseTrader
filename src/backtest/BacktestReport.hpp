#pragma once
// backtest_report.hpp — Human-readable + JSON report for one backtest run (M29)

#include "backtest/backtest_types.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace pulse::backtest
{

// ---------------------------------------------------------------------------
// BacktestReport — formats BacktestStats + load stats into a stdout table
// and a JSON export.
// ---------------------------------------------------------------------------
class BacktestReport
{
  public:
    /// The report decorates stats with run context (options, load stats,
    /// collected signals, warmup/feed counts).
    BacktestReport(const BacktestOptions &opts,
                   const BacktestStats &stats,
                   const KlineLoadStats &load,
                   const std::vector<BacktestSignal> &signals,
                   std::size_t warmup_candles,
                   std::size_t candles_fed);

    /// Plain-text report (std::cout style — caller chooses the stream).
    [[nodiscard]] std::string formatTable() const;

    /// Full machine-readable export (stats + trades + signals + equity
    /// curve + data provenance). Times are epoch ms; prices/PnL are doubles.
    [[nodiscard]] nlohmann::json toJson() const;

  private:
    BacktestOptions m_opts;
    BacktestStats m_stats;
    KlineLoadStats m_load;
    std::vector<BacktestSignal> m_signals;
    std::size_t m_warmupCandles = 0;
    std::size_t m_candlesFed = 0;
};

} // namespace pulse::backtest
