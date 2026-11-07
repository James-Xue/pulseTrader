#pragma once
// replay_driver.hpp — Historical candle replay through a real strategy (M29)
//
// Drives ONE registered strategy with the exact same code path the live
// engine uses: candles are pushed into the feed's KlineBuffer and onKline()
// runs the full template (warmup gate → evaluateEntry → cooldown → buildSignal
// → emitSignal with the min_confidence gate). The FeedHarness construction
// (null WS client, never started) keeps the replay fully offline.
//
// Signals are forwarded to a BacktestAccount for fill simulation, and a
// pristine copy is kept for the report (timestamped by candle open time —
// TradingSignal.timestamp is wall-clock and meaningless under fast replay).

#include "backtest/BacktestAccount.hpp"
#include "backtest/backtest_types.hpp"
#include "core/PulseError.hpp"
#include "exchange/GateRestClient.hpp"
#include "market/MarketFeed.hpp"
#include "strategy/StrategyBase.hpp"
#include "strategy/StrategyRegistry.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace pulse::backtest
{

// ---------------------------------------------------------------------------
// ReplayResult — what one replay pass produced
// ---------------------------------------------------------------------------
struct ReplayResult
{
    std::vector<BacktestSignal> signals;  ///< Non-Flat signals, in candle order.
    std::size_t candles_fed = 0;          ///< Candles pushed into the buffer.
    std::size_t warmup_candles = 0;       ///< Candles fed before the first signal
                                          ///< (equals the strategy warmup window when
                                          ///< the first evaluated candle signals).
    std::size_t evaluations = 0;          ///< Candles that passed the warmup gate.
};

// ---------------------------------------------------------------------------
// ReplayDriver — offline replay of one strategy instance
// ---------------------------------------------------------------------------
class ReplayDriver
{
  public:
    /// Builds the offline feed + strategy from options. `registry` must
    /// outlive this driver (provides the strategy factory).
    ReplayDriver(const BacktestOptions &opts, strategy::StrategyRegistry &registry);

    /// Replay `candles` (ascending by open_time) through the strategy,
    /// feeding fills into `account`. Returns the collected signals.
    [[nodiscard]] Result<ReplayResult> run(
        const std::vector<market::Kline> &candles, BacktestAccount &account);

  private:
    BacktestOptions m_opts;
    exchange::GateRestClient m_rest;
    market::MarketFeed m_feed;
    std::unique_ptr<strategy::StrategyBase> m_strategy;
    std::vector<BacktestSignal> m_signals;

    // Per-iteration context captured by the signal callback (synchronous).
    BacktestAccount *m_account = nullptr;
    std::int64_t m_currentCandleOpenMs = 0;
    std::int64_t m_currentCandleIndex = 0;
    std::int64_t m_firstSignalIndex = -1;
};

} // namespace pulse::backtest
