#pragma once
// multi_timeframe_ema.hpp — multi-timeframe EMA reference panel data
//
// Tracks the EMA stack {7, 14, 31, 60, 90, 120, 180} across seven timeframes
// {1m, 3m, 5m, 10m, 15m, 30m, 60m}, ordered by timeframe (1m → 60m). Each
// timeframe aggregates the polled ticks into its own candle series and
// backfills history via getCfdKlines on warmup.
//
// Display-only for now: the snapshot feeds the UI panel ("按时间排列显示");
// the values are also available for future strategy logic.
//
// Thread safety: NOT thread-safe — drive from the single polling thread.

#include <QString>

#include <cstdint>
#include <memory>
#include <vector>

namespace pulse::exchange
{
class GateRestClient;
}

namespace pulse::market
{
struct Kline;
}

namespace pulse::ui
{

/// One timeframe row: the EMA values for the configured periods.
struct EmaRow
{
    QString period;            ///< "1m" / "3m" / ... / "60m".
    bool available = false;    ///< False when the kline_type backfill failed
                               ///< (unsupported by the API) — hide the row.
    bool valid = false;        ///< False while warming up (< 180 candles).
    std::vector<double> emas;  ///< One value per period (same order).
};

class MultiTimeframeEma
{
  public:
    /// EMA periods, ordered fastest → slowest.
    static const std::vector<int> &periods();

    /// Timeframe labels in display order (1m → 60m); the last one maps to
    /// the Gate "1h" kline_type.
    static const std::vector<QString> &timeframes();

    MultiTimeframeEma();
    ~MultiTimeframeEma();  // Defined in the .cpp (Kline must be complete there).

    /// Backfill each timeframe (500 candles per kline_type). Returns the
    /// total candles loaded; a timeframe whose kline_type is unsupported
    /// warms up organically from live polls.
    int warmup(pulse::exchange::GateRestClient &cfd);

    /// Feed one observed price into every timeframe's candle.
    void onTick(double price, std::int64_t nowMs);

    /// Current EMA snapshot, one row per timeframe (display order).
    std::vector<EmaRow> snapshot() const;

    /// Candles buffered for a timeframe (0..7); diagnostics/tests.
    int candleCount(int timeframeIndex) const;

  private:
    struct Track
    {
        std::int64_t periodMs = 60'000;
        QString label;
        std::vector<pulse::market::Kline> klines;
        std::int64_t currentBucketMs = -1;
        bool hasBackfill = false;  ///< warmup() loaded history for this kline_type.
    };

    std::vector<Track> m_tracks;  ///< 1m · 3m · 5m · 10m · 15m · 30m · 60m.
};

} // namespace pulse::ui
