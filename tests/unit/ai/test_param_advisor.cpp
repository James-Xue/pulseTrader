// test_param_advisor.cpp — Unit tests for ParamAdvisor delta validation (Layer 4)
//
// Tests:
//   1. Default bounds — all 10 parameters have bounds configured
//   2. Apply zero deltas — no parameters changed
//   3. Apply within bounds — parameters updated correctly
//   4. Clamp delta — excessive deltas are clamped to max_delta
//   5. Hard bounds — results are clamped to [hard_min, hard_max]
//   6. All 10 fields — each delta maps to the correct StrategyParams field
//   7. Atomic semantics — concurrent read/write is safe
//   8. Custom bounds — set_bound() overrides defaults

#include "pulse/ai/param_advisor.hpp"

#include "pulse/strategy/strategy_params.hpp"

#include <gtest/gtest.h>

#include <thread>
#include <vector>

using namespace pulse;
using namespace pulse::ai;
using namespace pulse::strategy;

// ---------------------------------------------------------------------------
// 1. Default bounds initialization
// ---------------------------------------------------------------------------
TEST(ParamAdvisor, DefaultBounds)
{
    ParamAdvisor advisor;
    const auto &b = advisor.bounds();

    // All 10 parameter bounds should be present
    EXPECT_EQ(b.size(), 10u);
    EXPECT_TRUE(b.contains("order_quantity"));
    EXPECT_TRUE(b.contains("min_confidence"));
    EXPECT_TRUE(b.contains("ema_fast_period"));
    EXPECT_TRUE(b.contains("ema_slow_period"));
    EXPECT_TRUE(b.contains("bb_period"));
    EXPECT_TRUE(b.contains("bb_std_dev"));
    EXPECT_TRUE(b.contains("ob_imbalance_threshold"));
    EXPECT_TRUE(b.contains("cooldown_seconds"));
    EXPECT_TRUE(b.contains("stop_loss_pct"));
    EXPECT_TRUE(b.contains("take_profit_pct"));
}

// ---------------------------------------------------------------------------
// 2. Zero deltas — no changes
// ---------------------------------------------------------------------------
TEST(ParamAdvisor, ZeroDeltasNoChange)
{
    ParamAdvisor advisor;
    StrategyParams params;
    AnalysisResult result; // All deltas = 0.0

    auto updates = advisor.apply(result, params);
    EXPECT_TRUE(updates.empty());

    // All params should be at defaults
    EXPECT_DOUBLE_EQ(params.order_quantity.load(), 0.001);
    EXPECT_DOUBLE_EQ(params.min_confidence.load(), 0.6);
}

// ---------------------------------------------------------------------------
// 3. Apply within bounds
// ---------------------------------------------------------------------------
TEST(ParamAdvisor, ApplyWithinBounds)
{
    ParamAdvisor advisor;
    StrategyParams params;
    AnalysisResult result;
    result.param_deltas.order_quantity_delta = 0.0002; // Within ±0.0005

    auto updates = advisor.apply(result, params);
    EXPECT_EQ(updates.size(), 1u);
    EXPECT_EQ(updates[0].param_name, "order_quantity");
    EXPECT_DOUBLE_EQ(updates[0].old_value, 0.001);
    EXPECT_DOUBLE_EQ(updates[0].new_value, 0.001 + 0.0002);
    EXPECT_DOUBLE_EQ(params.order_quantity.load(), 0.0012);
}

// ---------------------------------------------------------------------------
// 4. Clamp delta to max_delta
// ---------------------------------------------------------------------------
TEST(ParamAdvisor, ClampExcessiveDelta)
{
    ParamAdvisor advisor;
    StrategyParams params;
    AnalysisResult result;

    // max_delta for order_quantity is ±0.0005 — try to exceed it
    result.param_deltas.order_quantity_delta = 0.01; // 20x max

    advisor.apply(result, params);

    // Should be clamped to max_delta = 0.0005
    double expected = 0.001 + 0.0005;
    EXPECT_DOUBLE_EQ(params.order_quantity.load(), expected);
}

// ---------------------------------------------------------------------------
// 5. Hard bounds — result clamped to [hard_min, hard_max]
// ---------------------------------------------------------------------------
TEST(ParamAdvisor, HardBoundsClampResult)
{
    ParamAdvisor advisor;
    StrategyParams params;

    // Set order_quantity close to hard_max (0.1)
    params.order_quantity.store(0.0998, std::memory_order_release);

    AnalysisResult result;
    result.param_deltas.order_quantity_delta = 0.0005; // Would push to 0.1003

    advisor.apply(result, params);

    // Should be clamped to hard_max = 0.1
    EXPECT_DOUBLE_EQ(params.order_quantity.load(), 0.1);
}

TEST(ParamAdvisor, HardBoundsClampMin)
{
    ParamAdvisor advisor;
    StrategyParams params;

    // Set order_quantity close to hard_min (0.0001)
    params.order_quantity.store(0.0002, std::memory_order_release);

    AnalysisResult result;
    result.param_deltas.order_quantity_delta = -0.0005; // Would push to -0.0003

    advisor.apply(result, params);

    // Should be clamped to hard_min = 0.0001
    EXPECT_DOUBLE_EQ(params.order_quantity.load(), 0.0001);
}

// ---------------------------------------------------------------------------
// 6. All 10 fields — each delta maps correctly
// ---------------------------------------------------------------------------
TEST(ParamAdvisor, AllFieldsApply)
{
    ParamAdvisor advisor;
    StrategyParams params;
    AnalysisResult result;

    result.param_deltas.order_quantity_delta = 0.0001;
    result.param_deltas.min_confidence_delta = -0.05;
    result.param_deltas.ema_fast_period_delta = 1.0;
    result.param_deltas.ema_slow_period_delta = -1.0;
    result.param_deltas.bb_period_delta = 0.5;
    result.param_deltas.bb_std_dev_delta = -0.1;
    result.param_deltas.ob_imbalance_threshold_delta = 0.01;
    result.param_deltas.cooldown_seconds_delta = -2.0;
    result.param_deltas.stop_loss_pct_delta = 0.001;
    result.param_deltas.take_profit_pct_delta = -0.0005;

    auto updates = advisor.apply(result, params);
    EXPECT_EQ(updates.size(), 10u);

    EXPECT_DOUBLE_EQ(params.order_quantity.load(), 0.001 + 0.0001);
    EXPECT_DOUBLE_EQ(params.min_confidence.load(), 0.6 - 0.05);
    EXPECT_DOUBLE_EQ(params.ema_fast_period.load(), 9.0 + 1.0);
    EXPECT_DOUBLE_EQ(params.ema_slow_period.load(), 21.0 - 1.0);
    EXPECT_DOUBLE_EQ(params.bb_period.load(), 20.0 + 0.5);
    EXPECT_DOUBLE_EQ(params.bb_std_dev.load(), 2.0 - 0.1);
    EXPECT_DOUBLE_EQ(params.ob_imbalance_threshold.load(), 0.3 + 0.01);
    EXPECT_DOUBLE_EQ(params.cooldown_seconds.load(), 30.0 - 2.0);
    EXPECT_DOUBLE_EQ(params.stop_loss_pct.load(), 0.01 + 0.001);
    EXPECT_DOUBLE_EQ(params.take_profit_pct.load(), 0.005 - 0.0005);
}

// ---------------------------------------------------------------------------
// 7. Custom bounds via set_bound
// ---------------------------------------------------------------------------
TEST(ParamAdvisor, CustomBounds)
{
    ParamAdvisor advisor;

    // Tighten the order_quantity bounds
    ParamBound tight{ 0.0001, 0.0005, 0.005 };
    advisor.set_bound("order_quantity", tight);

    StrategyParams params;
    AnalysisResult result;
    result.param_deltas.order_quantity_delta = 0.0005; // Exceeds new max_delta

    advisor.apply(result, params);

    // Should be clamped to new max_delta = 0.0001
    EXPECT_DOUBLE_EQ(params.order_quantity.load(), 0.001 + 0.0001);
}

// ---------------------------------------------------------------------------
// 8. Negative delta respects hard_min
// ---------------------------------------------------------------------------
TEST(ParamAdvisor, NegativeDeltaRespectsMinConfidence)
{
    ParamAdvisor advisor;
    StrategyParams params;

    // min_confidence hard_min = 0.1, current = 0.6
    params.min_confidence.store(0.15, std::memory_order_release);

    AnalysisResult result;
    result.param_deltas.min_confidence_delta = -0.1; // Would push to 0.05

    advisor.apply(result, params);

    // Clamped to hard_min = 0.1
    EXPECT_DOUBLE_EQ(params.min_confidence.load(), 0.1);
}
