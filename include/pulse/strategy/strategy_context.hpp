#pragma once
// strategy_context.hpp — Dependency injection bundle (Layer 6 Strategy Engine)
//
// Holds pointers/references to all upstream and downstream components a strategy
// needs. Constructed once by StrategyManager and passed to each strategy at
// creation time.
//
// Components:
//   - MarketFeed (L3)    — read-only access to ticker, kline, orderbook
//   - RiskManager (L7)   — evaluate proposed orders against risk rules
//   - OrderExecutor (L8) — place orders on the exchange
//   - StrategyInstanceConfig — per-strategy runtime configuration

#include "pulse/core/config.hpp"
#include "pulse/execution/order_executor.hpp"
#include "pulse/market/market_feed.hpp"
#include "pulse/risk/risk_manager.hpp"

namespace pulse::strategy
{

// ---------------------------------------------------------------------------
// StrategyContext — injected dependency bundle for strategy instances
//
// Constructed by StrategyManager before creating each strategy.
// All pointers are non-owning; lifetime managed by the application layer.
// ---------------------------------------------------------------------------
struct StrategyContext
{
    market::MarketFeed *market_feed = nullptr;           ///< L3 market data (read-only).
    risk::RiskManager *risk_manager = nullptr;           ///< L7 risk gate.
    execution::OrderExecutor *order_executor = nullptr;  ///< L8 order placement.
    StrategyInstanceConfig config;                       ///< Per-strategy config copy.

    /// Default constructor.
    StrategyContext()
        : market_feed{ nullptr }
        , risk_manager{ nullptr }
        , order_executor{ nullptr }
        , config{}
    {
    }

    /// Construct with all dependencies.
    ///
    /// Parameters:
    ///   1. feed     — L3 market data provider
    ///   2. risk     — L7 risk evaluation engine
    ///   3. executor — L8 order executor
    ///   4. cfg      — strategy-specific configuration
    StrategyContext(market::MarketFeed &feed,
        risk::RiskManager &risk,
        execution::OrderExecutor &executor,
        const StrategyInstanceConfig &cfg)
        : market_feed{ &feed }
        , risk_manager{ &risk }
        , order_executor{ &executor }
        , config{ cfg }
    {
    }
};

} // namespace pulse::strategy
