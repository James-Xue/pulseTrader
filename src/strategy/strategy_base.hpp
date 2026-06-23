#pragma once
// strategy_base.hpp — Abstract base class for all strategies (Layer 6 Strategy Engine)
//
// Defines the lifecycle hooks that concrete strategies must implement.
// StrategyManager creates one std::jthread per strategy; the thread polls
// market data and calls these hooks at appropriate intervals.
//
// Lifecycle:
//   1. onTick(ticker)       — every best-bid/ask update (highest frequency)
//   2. onOrderbook(book)    — when top N levels change
//   3. onKline(kline)       — on candle close (lowest frequency on hot path)
//
// Signal emission:
//   - Concrete strategies call emitSignal() when they detect an opportunity
//   - StrategyManager sets a callback that forwards to SignalAggregator
//
// Thread safety:
//   - Each strategy runs on its own std::jthread (started by StrategyManager)
//   - m_active is atomic<bool> for cooperative stop checking
//   - m_params is atomic for lock-free AI hot-reload

#include "core/types.hpp"
#include "market/kline_buffer.hpp"
#include "market/orderbook_manager.hpp"
#include "market/ticker_cache.hpp"
#include "strategy/signal_types.hpp"
#include "strategy/strategy_context.hpp"
#include "strategy/strategy_params.hpp"

#include <atomic>
#include <functional>
#include <string>

namespace pulse::strategy
{

// ---------------------------------------------------------------------------
// StrategyBase — abstract base class for all trading strategies
// ---------------------------------------------------------------------------
class StrategyBase
{
  public:
    /// Callback type for signal emission.
    using SignalCallback = std::function<void(const TradingSignal &)>;

    virtual ~StrategyBase() = default;

    /// Human-readable strategy class name (e.g. "MomentumScalper").
    [[nodiscard]] virtual std::string name() const = 0;

    /// Unique strategy instance ID (e.g. "momentum_scalper_BTC_USDT").
    [[nodiscard]] virtual std::string id() const = 0;

    /// Mutable access to hot-reloadable parameters.
    /// AI layer writes here; strategy reads on hot path.
    [[nodiscard]] virtual StrategyParams &params() = 0;

    /// Access the injected context (used by StrategyManager).
    [[nodiscard]] const StrategyContext &context() const;
    [[nodiscard]] StrategyContext &context();

    // --- Lifecycle hooks (called from strategy thread) ---

    /// Called on each ticker update (highest frequency).
    ///
    /// Parameters:
    ///   1. ticker — latest best-bid/ask snapshot for this strategy's symbol
    virtual void onTick(const market::Ticker &ticker) = 0;

    /// Called on each closed K-line candle.
    ///
    /// Parameters:
    ///   1. kline — the just-closed candle
    virtual void onKline(const market::Kline &kline) = 0;

    /// Called when the order book top N levels change.
    ///
    /// Parameters:
    ///   1. book — current order book snapshot
    virtual void onOrderbook(const market::OrderBook &book) = 0;

    // --- Signal emission ---

    /// Set the callback that receives emitted signals.
    /// Called once by StrategyManager during registration.
    void setSignalCallback(SignalCallback cb);

    /// Atomic flag for cooperative stop (checked by strategy thread).
    [[nodiscard]] std::atomic<bool> &active();

  protected:
    /// Emit a trading signal to the registered callback.
    ///
    /// If confidence < min_confidence or no callback is set, the signal
    /// is silently dropped.
    ///
    /// Parameters:
    ///   1. signal — the signal to emit (type, symbol, confidence, reason)
    void emitSignal(const TradingSignal &signal);

    StrategyContext m_context;   ///< Injected dependencies (market, risk, execution).
    SignalCallback m_signalCallback; ///< Where emitted signals are forwarded.
    std::atomic<bool> m_active{ false }; ///< Cooperative stop flag.
};

} // namespace pulse::strategy
