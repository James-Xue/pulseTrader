#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QVector>

#include "data/DataServerConfig.hpp"

namespace pulse::client::data
{

/// Stateless technical-indicator math for the local data server.
///
/// Input is the engine's kline array (oldest first, each object carrying the
/// keys from pulse::market::Kline). Only candles whose `closed` flag is true
/// take part in any computation: the in-progress candle still moves, and using
/// it would make every published value flicker between polls.
///
/// Thread affinity: none. The class owns no Qt object and no mutable state, so
/// it may be called from any thread. The data server calls it on the GUI
/// thread, where 500 bars cost microseconds.
class IndicatorEngine
{
public:
    /// Periods and smoothing constants. The defaults are DATA IPC PROTOCOL v1
    /// values; tests shrink them to keep hand-computed vectors short.
    struct Params
    {
        QList<int> emaPeriods{ 7, 14, 30, 60, 200 };
        int macdFast = 12;
        int macdSlow = 26;
        int macdSignal = 9;
        int atrPeriod = 10;
        double supertrendMultiplier = 3.0;
    };

    /// Protocol-default periods. A default argument cannot be used here: the
    /// nested Params type is not complete until the class is, so `Params{}`
    /// as a default argument is ill-formed.
    IndicatorEngine();

    /// Explicit periods, used by tests that need short hand-checkable vectors.
    explicit IndicatorEngine(Params params);

    [[nodiscard]] const Params& params() const
    {
        return m_params;
    }

    /// Compute the protocol's indicators object:
    ///   {"ema":{"7":x,...},
    ///    "macd":{"line":x,"signal":x,"hist":x},
    ///    "supertrend":{"value":x,"direction":"up"|"down"}}
    ///
    /// Every value that needs more history than the input provides is JSON
    /// null. A zero would be indistinguishable from a real computed zero.
    [[nodiscard]] QJsonObject compute(const QJsonArray& klines) const;

    /// Exponential moving average seeded with the SMA of the first `period`
    /// values, then smoothed with k = 2 / (period + 1).
    ///
    /// Returns one entry per input value. Entries before the seed index
    /// (period - 1) are NaN, which signals "not defined yet".
    [[nodiscard]] static QVector<double> emaSeries(const QVector<double>& values,
                                                  int period);

private:
    Params m_params;
};

} // namespace pulse::client::data
