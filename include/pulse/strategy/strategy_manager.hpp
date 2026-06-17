#pragma once
// strategy_manager.hpp — Multi-strategy orchestration (Layer 6 Strategy Engine)
//
// Manages the lifecycle of all registered strategies:
//   1. register_strategy() — add a strategy (takes ownership)
//   2. start()             — spawn one std::jthread per enabled strategy
//   3. stop()              — signal all threads to stop and join
//
// Each strategy thread polls market data at its configured interval and
// calls the appropriate lifecycle hooks (on_tick, on_kline, on_orderbook).
//
// Thread topology:
//   - One std::jthread per active strategy (cooperative cancellation via stop_token)
//   - Strategy threads read market data lock-free (seqlock / shared_mutex)
//   - Signals are forwarded to a user-provided callback (typically SignalAggregator)
//
// Thread safety:
//   - register_strategy() must be called before start()
//   - start() / stop() are not thread-safe (call from main thread only)
//   - signal_callback_ is set once before threads start (no race)

#include "pulse/strategy/strategy_base.hpp"

#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace pulse::strategy
{

// ---------------------------------------------------------------------------
// StrategySnapshot — lightweight read-only snapshot of one strategy's state
//
// Used by the WebUI dashboard (Layer 9) to display strategy status without
// accessing the internal strategy vector.
// ---------------------------------------------------------------------------
struct StrategySnapshot
{
    std::string name;             ///< Strategy class name (e.g. "MomentumScalper").
    std::string id;               ///< Unique instance ID (e.g. "momentum_scalper_BTC_USDT").
    std::string symbol;           ///< Trading pair.
    bool enabled;                 ///< Whether the strategy is configured as enabled.
    bool running;                 ///< Whether the strategy thread is currently active.
    std::uint32_t poll_interval_ms; ///< Poll interval.

    StrategySnapshot()
        : name{}
        , id{}
        , symbol{}
        , enabled{ false }
        , running{ false }
        , poll_interval_ms{ 0 }
    {
    }
};

// ---------------------------------------------------------------------------
// StrategyManager — spawns and manages strategy threads
// ---------------------------------------------------------------------------
class StrategyManager
{
  public:
    /// Callback type for forwarding aggregated signals to Risk/Execution.
    using SignalCallback = std::function<void(const TradingSignal &)>;

    StrategyManager() = default;
    ~StrategyManager();

    // Non-copyable, non-movable (owns threads).
    StrategyManager(const StrategyManager &) = delete;
    StrategyManager &operator=(const StrategyManager &) = delete;
    StrategyManager(StrategyManager &&) = delete;
    StrategyManager &operator=(StrategyManager &&) = delete;

    /// Register a strategy (takes ownership via unique_ptr).
    ///
    /// Must be called before start(). The strategy's signal callback
    /// is wired to the manager's internal forwarding callback.
    ///
    /// Parameters:
    ///   1. strategy — the strategy instance to register
    void register_strategy(std::unique_ptr<StrategyBase> strategy);

    /// Set the callback that receives all emitted signals.
    ///
    /// Typically wired to SignalAggregator::add_signal().
    /// Must be called before start().
    void set_signal_callback(SignalCallback cb);

    /// Start all enabled strategies — one std::jthread each.
    ///
    /// Each thread:
    ///   1. Polls ticker cache for the strategy's symbol
    ///   2. Checks for new closed klines → calls on_kline()
    ///   3. Checks for orderbook updates → calls on_orderbook()
    ///   4. Calls on_tick() with latest ticker
    ///   5. Sleeps for poll_interval_ms
    ///   6. Checks stop_token for cooperative cancellation
    void start();

    /// Stop all strategy threads — signals stop and joins.
    void stop();

    /// Number of registered strategies.
    [[nodiscard]] std::size_t strategy_count() const;

    /// Number of currently running strategy threads.
    [[nodiscard]] std::size_t running_count() const;

    /// Returns a snapshot of all registered strategies (read-only, thread-safe).
    ///
    /// Iterates the internal strategy vector and captures each strategy's
    /// name, id, symbol, enabled flag, and running state.
    /// Safe to call from the WebUI thread while strategy threads are running.
    [[nodiscard]] std::vector<StrategySnapshot> snapshot() const;

  private:
    std::vector<std::unique_ptr<StrategyBase>> strategies_;
    std::vector<std::jthread> threads_;
    SignalCallback signal_callback_;

    /// The main loop for a single strategy thread.
    ///
    /// Parameters:
    ///   1. strategy — the strategy to run
    ///   2. stoken   — stop_token for cooperative cancellation
    void strategy_loop(StrategyBase &strategy, std::stop_token stoken);
};

} // namespace pulse::strategy
