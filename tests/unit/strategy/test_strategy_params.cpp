// test_strategy_params.cpp — Unit tests for hot-reload strategy params (Layer 6 Strategy Engine)

#include "pulse/strategy/strategy_params.hpp"

#include <gtest/gtest.h>

#include <thread>
#include <vector>

using namespace pulse::strategy;

// ---------------------------------------------------------------------------
// Default values
// ---------------------------------------------------------------------------

TEST(StrategyParams, DefaultValues)
{
    StrategyParams params;

    EXPECT_DOUBLE_EQ(0.001, params.order_quantity.load());
    EXPECT_DOUBLE_EQ(0.6, params.min_confidence.load());
    EXPECT_DOUBLE_EQ(9.0, params.ema_fast_period.load());
    EXPECT_DOUBLE_EQ(21.0, params.ema_slow_period.load());
    EXPECT_DOUBLE_EQ(20.0, params.bb_period.load());
    EXPECT_DOUBLE_EQ(2.0, params.bb_std_dev.load());
    EXPECT_DOUBLE_EQ(0.3, params.ob_imbalance_threshold.load());
    EXPECT_DOUBLE_EQ(5.0, params.ob_depth.load());
    EXPECT_DOUBLE_EQ(30.0, params.cooldown_seconds.load());
}

// ---------------------------------------------------------------------------
// Atomic read/write
// ---------------------------------------------------------------------------

TEST(StrategyParams, AtomicReadWrite)
{
    StrategyParams params;

    // Write with release semantics (AI layer pattern).
    params.order_quantity.store(0.01, std::memory_order_release);
    params.min_confidence.store(0.8, std::memory_order_release);
    params.ema_fast_period.store(12.0, std::memory_order_release);

    // Read with acquire semantics (strategy thread pattern).
    EXPECT_DOUBLE_EQ(0.01, params.order_quantity.load(std::memory_order_acquire));
    EXPECT_DOUBLE_EQ(0.8, params.min_confidence.load(std::memory_order_acquire));
    EXPECT_DOUBLE_EQ(12.0, params.ema_fast_period.load(std::memory_order_acquire));
}

// ---------------------------------------------------------------------------
// Concurrent access (lock-free)
// ---------------------------------------------------------------------------

TEST(StrategyParams, ConcurrentAccess)
{
    StrategyParams params;
    params.order_quantity.store(0.001, std::memory_order_release);

    constexpr int kIterations = 10'000;
    std::atomic<bool> stop{ false };

    // Writer thread (simulating AI layer).
    std::thread writer([&]()
        {
            for (int i = 0; i < kIterations && !stop.load(); ++i)
            {
                params.order_quantity.store(0.001 + i * 0.0001, std::memory_order_release);
            }
        });

    // Reader thread (simulating strategy hot path).
    std::thread reader([&]()
        {
            for (int i = 0; i < kIterations && !stop.load(); ++i)
            {
                double val = params.order_quantity.load(std::memory_order_acquire);
                // Value should always be positive (never corrupted).
                EXPECT_GT(val, 0.0);
            }
        });

    writer.join();
    reader.join();
}

// ---------------------------------------------------------------------------
// Multiple instances are independent
// ---------------------------------------------------------------------------

TEST(StrategyParams, IndependentInstances)
{
    StrategyParams params_a;
    StrategyParams params_b;

    params_a.order_quantity.store(0.05, std::memory_order_release);
    params_b.order_quantity.store(0.10, std::memory_order_release);

    EXPECT_DOUBLE_EQ(0.05, params_a.order_quantity.load());
    EXPECT_DOUBLE_EQ(0.10, params_b.order_quantity.load());
}
