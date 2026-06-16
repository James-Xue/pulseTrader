// test_order_tracker.cpp — Unit tests for OrderTracker (Layer 8 Order Execution)

#include "pulse/execution/order_tracker.hpp"

#include <gtest/gtest.h>

using namespace pulse;
using namespace pulse::execution;

// ---------------------------------------------------------------------------
// OrderTracker helper methods
// ---------------------------------------------------------------------------

TEST(OrderTracker, IsTerminalStatusFilled)
{
    EXPECT_TRUE(OrderTracker::is_terminal_status(OrderStatus::Filled));
}

TEST(OrderTracker, IsTerminalStatusCancelled)
{
    EXPECT_TRUE(OrderTracker::is_terminal_status(OrderStatus::Cancelled));
}

TEST(OrderTracker, IsTerminalStatusOpen)
{
    EXPECT_FALSE(OrderTracker::is_terminal_status(OrderStatus::Open));
}

TEST(OrderTracker, IsTerminalStatusPending)
{
    EXPECT_FALSE(OrderTracker::is_terminal_status(OrderStatus::Pending));
}

TEST(OrderTracker, ParseStatusOpen)
{
    EXPECT_EQ(OrderTracker::parse_status("open"), OrderStatus::Open);
}

TEST(OrderTracker, ParseStatusClosed)
{
    EXPECT_EQ(OrderTracker::parse_status("closed"), OrderStatus::Filled);
}

TEST(OrderTracker, ParseStatusCancelled)
{
    EXPECT_EQ(OrderTracker::parse_status("cancelled"), OrderStatus::Cancelled);
}

TEST(OrderTracker, ParseStatusUnknown)
{
    EXPECT_EQ(OrderTracker::parse_status("unknown"), OrderStatus::Pending);
}

// ---------------------------------------------------------------------------
// OrderTracker (requires WS + REST clients — tested via integration tests)
// ---------------------------------------------------------------------------

// Note: Full OrderTracker testing requires real or mock WS/REST clients.
// Integration tests in tools/test_execution.cpp will cover:
// - track_order() and WS subscription
// - on_order_update() state machine
// - poll_order_status() REST fallback
// - ExecutionReport generation
// - Completion callback invocation

// Placeholder for future mock-based unit tests:
// TEST(OrderTracker, TrackOrderSubscribesToWs)
// TEST(OrderTracker, OnOrderUpdateOpenToPartiallyFilled)
// TEST(OrderTracker, OnOrderUpdatePartiallyFilledToFilled)
// TEST(OrderTracker, GenerateReportCalculatesSlippage)
// TEST(OrderTracker, CompletionCallbackInvoked)
