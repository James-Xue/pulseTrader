// test_execution_report.cpp — Unit tests for ExecutionReport (Layer 8 Order Execution)

#include "execution/execution_report.hpp"

#include <gtest/gtest.h>

#include <chrono>

using namespace pulse;
using namespace pulse::execution;

// ---------------------------------------------------------------------------
// Construction and default values
// ---------------------------------------------------------------------------

TEST(ExecutionReport, DefaultConstruction)
{
    ExecutionReport report;
    EXPECT_EQ(report.order_id, "");
    EXPECT_EQ(report.symbol, "");
    EXPECT_EQ(report.side, Side::Buy);
    EXPECT_EQ(report.type, OrderType::Limit);
    EXPECT_DOUBLE_EQ(report.requested_qty, 0.0);
    EXPECT_DOUBLE_EQ(report.filled_qty, 0.0);
    EXPECT_DOUBLE_EQ(report.avg_fill_price, 0.0);
    EXPECT_DOUBLE_EQ(report.slippage_bps, 0.0);
    EXPECT_DOUBLE_EQ(report.fees, 0.0);
    EXPECT_EQ(report.latency.count(), 0);
    EXPECT_EQ(report.final_status, OrderStatus::Pending);
}

TEST(ExecutionReport, ManualConstruction)
{
    ExecutionReport report;
    report.order_id = "12345";
    report.client_order_id = "client_001";
    report.symbol = "BTC_USDT";
    report.side = Side::Buy;
    report.type = OrderType::Limit;
    report.requested_qty = 0.001;
    report.filled_qty = 0.001;
    report.avg_fill_price = 50001.0;
    report.submit_mid_price = 50000.0;
    report.slippage_bps = 2.0;
    report.fees = 0.05;
    report.latency = std::chrono::milliseconds{ 150 };
    report.submit_time = now();
    report.fill_time = report.submit_time + std::chrono::milliseconds{ 150 };
    report.final_status = OrderStatus::Filled;

    EXPECT_EQ(report.order_id, "12345");
    EXPECT_EQ(report.symbol, "BTC_USDT");
    EXPECT_DOUBLE_EQ(report.filled_qty, 0.001);
    EXPECT_EQ(report.final_status, OrderStatus::Filled);
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

TEST(ExecutionReport, ToJson)
{
    ExecutionReport report;
    report.order_id = "12345";
    report.client_order_id = "client_001";
    report.symbol = "BTC_USDT";
    report.side = Side::Buy;
    report.type = OrderType::Limit;
    report.requested_qty = 0.001;
    report.filled_qty = 0.001;
    report.avg_fill_price = 50001.0;
    report.submit_mid_price = 50000.0;
    report.slippage_bps = 2.0;
    report.fees = 0.05;
    report.latency = std::chrono::milliseconds{ 150 };
    report.final_status = OrderStatus::Filled;

    const auto json = report.to_json();

    EXPECT_EQ(json["order_id"], "12345");
    EXPECT_EQ(json["client_order_id"], "client_001");
    EXPECT_EQ(json["symbol"], "BTC_USDT");
    EXPECT_EQ(json["side"], "buy");
    EXPECT_EQ(json["type"], "limit");
    EXPECT_DOUBLE_EQ(json["requested_qty"], 0.001);
    EXPECT_DOUBLE_EQ(json["filled_qty"], 0.001);
    EXPECT_DOUBLE_EQ(json["avg_fill_price"], 50001.0);
    EXPECT_DOUBLE_EQ(json["slippage_bps"], 2.0);
    EXPECT_DOUBLE_EQ(json["fees"], 0.05);
    EXPECT_EQ(json["latency_ms"], 150);
    EXPECT_EQ(json["final_status"], "filled");
}

TEST(ExecutionReport, ToJsonSellOrder)
{
    ExecutionReport report;
    report.side = Side::Sell;
    report.type = OrderType::Market;
    report.final_status = OrderStatus::Cancelled;

    const auto json = report.to_json();

    EXPECT_EQ(json["side"], "sell");
    EXPECT_EQ(json["type"], "market");
    EXPECT_EQ(json["final_status"], "cancelled");
}

TEST(ExecutionReport, ToJsonPostOnlyOrder)
{
    ExecutionReport report;
    report.type = OrderType::PostOnly;

    const auto json = report.to_json();

    EXPECT_EQ(json["type"], "post_only");
}

// ---------------------------------------------------------------------------
// Slippage calculation
// ---------------------------------------------------------------------------

TEST(ExecutionReport, CalculateSlippageBuyPositive)
{
    // Buy order: fill at 50010, mid was 50000 → +2 bps (worse fill)
    const Price slippage = ExecutionReport::calculateSlippageBps(50010.0, 50000.0, Side::Buy);
    EXPECT_NEAR(slippage, 2.0, 0.1);
}

TEST(ExecutionReport, CalculateSlippageBuyNegative)
{
    // Buy order: fill at 49990, mid was 50000 → -2 bps (better fill)
    const Price slippage = ExecutionReport::calculateSlippageBps(49990.0, 50000.0, Side::Buy);
    EXPECT_NEAR(slippage, -2.0, 0.1);
}

TEST(ExecutionReport, CalculateSlippageSellPositive)
{
    // Sell order: fill at 50010, mid was 50000 → +2 bps (better fill, inverted)
    const Price slippage = ExecutionReport::calculateSlippageBps(50010.0, 50000.0, Side::Sell);
    EXPECT_NEAR(slippage, -2.0, 0.1); // Inverted for sell
}

TEST(ExecutionReport, CalculateSlippageSellNegative)
{
    // Sell order: fill at 49990, mid was 50000 → -2 bps (worse fill, inverted)
    const Price slippage = ExecutionReport::calculateSlippageBps(49990.0, 50000.0, Side::Sell);
    EXPECT_NEAR(slippage, 2.0, 0.1); // Inverted for sell
}

TEST(ExecutionReport, CalculateSlippageZeroMidPrice)
{
    // Edge case: mid_price = 0 → slippage = 0 (avoid division by zero)
    const Price slippage = ExecutionReport::calculateSlippageBps(50000.0, 0.0, Side::Buy);
    EXPECT_DOUBLE_EQ(slippage, 0.0);
}

TEST(ExecutionReport, CalculateSlippageZeroFillPrice)
{
    // Edge case: fill_price = 0 → slippage = 0
    const Price slippage = ExecutionReport::calculateSlippageBps(0.0, 50000.0, Side::Buy);
    EXPECT_DOUBLE_EQ(slippage, 0.0);
}
