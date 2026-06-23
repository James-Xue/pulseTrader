#pragma once
// signal_types.hpp — Trading signal definitions (Layer 6 Strategy Engine)
//
// Defines the signal types emitted by strategies and consumed by the
// SignalAggregator and downstream Risk/Execution layers.
//
// SignalType:
//   1. Buy  — strategy detects bullish opportunity
//   2. Sell — strategy detects bearish opportunity
//   3. Flat — no actionable signal (hold / neutral)
//
// TradingSignal:
//   - Emitted by StrategyBase::emitSignal()
//   - Aggregated by SignalAggregator
//   - Carries confidence (0.0–1.0) for weighted voting
//   - Carries strategy_id for traceability
//   - Carries market_type for routing to correct executor

#include "core/types.hpp"

#include <cstdint>
#include <string>

namespace pulse::strategy
{

// ---------------------------------------------------------------------------
// SignalType — direction of a trading signal
// ---------------------------------------------------------------------------
enum class SignalType : std::uint8_t
{
    Buy,  ///< Bullish signal — suggests opening a long position.
    Sell, ///< Bearish signal — suggests opening a short / closing long.
    Flat, ///< No actionable signal — neutral / hold.
};

// ---------------------------------------------------------------------------
// TradingSignal — emitted by a strategy when it detects an opportunity
//
// Fields:
//   1. type        — Buy / Sell / Flat
//   2. symbol      — trading pair this signal applies to
//   3. confidence  — signal strength (0.0 = noise, 1.0 = maximum conviction)
//   4. price       — reference price at signal generation time
//   5. strategy_id — unique identifier of the emitting strategy
//   6. timestamp   — when the signal was generated
//   7. reason      — human-readable explanation for logging / debugging
// ---------------------------------------------------------------------------
struct TradingSignal
{
    SignalType type;            ///< Signal direction.
    Symbol symbol;              ///< Trading pair (e.g. "BTC_USDT").
    double confidence;          ///< Signal strength in [0.0, 1.0].
    Price price;                ///< Reference price at generation time.
    std::string strategy_id;    ///< Unique ID of the emitting strategy.
    Timestamp timestamp;        ///< When the signal was generated.
    std::string reason;         ///< Human-readable explanation.
    MarketType market_type;     ///< Spot or Futures (for routing to correct executor).

    /// Default constructor.
    TradingSignal()
        : type{ SignalType::Flat }
        , symbol{}
        , confidence{ 0.0 }
        , price{ 0.0 }
        , strategy_id{}
        , timestamp{}
        , reason{}
        , market_type{ MarketType::Spot }
    {
    }
};

} // namespace pulse::strategy
