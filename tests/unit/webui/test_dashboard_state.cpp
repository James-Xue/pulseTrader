// test_dashboard_state.cpp — Unit tests for DashboardState polling aggregator (Layer 9 WebUI)
//
// Tests the tiered polling loop that assembles DashboardSnapshot from upstream
// components. Uses real component instances with empty/mock configurations.
//
// Component construction approach:
//   - GateWsClient / GateRestClient: constructed with empty ExchangeConfig (never started)
//   - MarketFeed: constructed with above clients (never started); ticker_cache and
//     orderbook_manager are populated directly for tests
//   - StrategyManager: constructed empty; strategies registered for specific tests
//   - RiskManager: constructed with PositionManager, DrawdownGuard, OrderRateLimiter
//   - OrderTracker: constructed with empty WS/REST clients
//   - AiPipeline: constructed with mock HttpTransport
//
// Tests:
//   1. DefaultStateIsNotRunning
//   2. StartSetsRunning
//   3. StopClearsRunning
//   4. LatestReturnsNullBeforePoll
//   5. PollProducesSnapshot
//   6. SnapshotHasTimestamp
//   7. PollFastPopulatesOrderBooks
//   8. PollMediumPopulatesPositions
//   9. PollSlowPopulatesStrategies
//  10. PollAiPopulatesWhenAvailable
//  11. SnapshotCallbackInvoked
//  12. StopTokenCancelsPollLoop

#include "pulse/webui/dashboard_state.hpp"

#include "pulse/ai/ai_pipeline.hpp"
#include "pulse/core/config.hpp"
#include "pulse/exchange/gate_rest_client.hpp"
#include "pulse/exchange/gate_ws_client.hpp"
#include "pulse/execution/order_tracker.hpp"
#include "pulse/market/market_feed.hpp"
#include "pulse/risk/drawdown_guard.hpp"
#include "pulse/risk/order_rate_limiter.hpp"
#include "pulse/risk/position_manager.hpp"
#include "pulse/risk/risk_manager.hpp"
#include "pulse/strategy/strategy_manager.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

using namespace pulse;
using namespace pulse::webui;
using namespace pulse::market;
using namespace pulse::strategy;
using namespace pulse::risk;
using namespace pulse::execution;
using namespace pulse::ai;
using namespace pulse::exchange;

// ---------------------------------------------------------------------------
// Test fixture — constructs all upstream components with empty/mock configs
// ---------------------------------------------------------------------------

class DashboardStateTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        // 1. WS/REST clients with empty config (never started, no network).
        ExchangeConfig exchange_config;
        ws_client_ = std::make_unique<GateWsClient>(exchange_config);
        rest_client_ = std::make_unique<GateRestClient>(exchange_config);

        // 2. MarketFeed (not started — components populated directly).
        market_feed_ = std::make_unique<MarketFeed>(*ws_client_, *rest_client_);

        // 3. StrategyManager (empty — strategies registered per-test).
        strategy_mgr_ = std::make_unique<StrategyManager>();

        // 4. Risk components.
        risk_config_ = RiskConfig{};
        position_mgr_ = std::make_unique<PositionManager>(risk_config_);
        drawdown_guard_ = std::make_unique<DrawdownGuard>(risk_config_);
        rate_limiter_ = std::make_unique<OrderRateLimiter>(risk_config_.maxOrdersPerSec);
        risk_mgr_ = std::make_unique<RiskManager>(
            risk_config_, *position_mgr_, *drawdown_guard_, *rate_limiter_);

        // 5. OrderTracker with empty WS/REST clients.
        order_tracker_ = std::make_unique<OrderTracker>(*ws_client_, *rest_client_);

        // 6. AiPipeline with mock transport (never actually called in these tests).
        AiConfig ai_config;
        ai_config.backend = "claude";
        ai_config.model = "claude-sonnet-4-6";
        ai_config.maxRetries = 0;

        TwitterConfig tw_config;
        tw_config.enabled = false;

        NewsConfig news_config;
        news_config.enabled = false;

        auto mock_transport = [](const std::string &, const std::string &,
                                  const std::vector<std::string> &) -> Result<nlohmann::json>
        {
            return PulseError{ ErrorCode::HttpError, "mock transport" };
        };

        ai_pipeline_ = std::make_unique<AiPipeline>(
            ai_config, tw_config, news_config, mock_transport);

        // 7. WebUI config.
        webui_config_ = WebUiConfig{};

        // 8. DashboardState under test.
        state_ = std::make_unique<DashboardState>(
            webui_config_,
            *market_feed_,
            *strategy_mgr_,
            *risk_mgr_,
            *order_tracker_,
            *ai_pipeline_);
    }

    void TearDown() override
    {
        // Ensure the state is stopped before tearing down.
        if (state_)
        {
            state_->stop();
        }
    }

    // Components (owned by fixture).
    std::unique_ptr<GateWsClient> ws_client_;
    std::unique_ptr<GateRestClient> rest_client_;
    std::unique_ptr<MarketFeed> market_feed_;
    std::unique_ptr<StrategyManager> strategy_mgr_;
    RiskConfig risk_config_;
    std::unique_ptr<PositionManager> position_mgr_;
    std::unique_ptr<DrawdownGuard> drawdown_guard_;
    std::unique_ptr<OrderRateLimiter> rate_limiter_;
    std::unique_ptr<RiskManager> risk_mgr_;
    std::unique_ptr<OrderTracker> order_tracker_;
    std::unique_ptr<AiPipeline> ai_pipeline_;
    WebUiConfig webui_config_;
    std::unique_ptr<DashboardState> state_;
};

// ---------------------------------------------------------------------------
// 1. DefaultStateIsNotRunning
// ---------------------------------------------------------------------------

TEST_F(DashboardStateTest, DefaultStateIsNotRunning)
{
    // A freshly constructed DashboardState must not be running.
    EXPECT_FALSE(state_->running());
}

// ---------------------------------------------------------------------------
// 2. StartSetsRunning
// ---------------------------------------------------------------------------

TEST_F(DashboardStateTest, StartSetsRunning)
{
    state_->start();

    // Give the thread a moment to start.
    std::this_thread::sleep_for(std::chrono::milliseconds{ 100 });

    EXPECT_TRUE(state_->running());

    state_->stop();
}

// ---------------------------------------------------------------------------
// 3. StopClearsRunning
// ---------------------------------------------------------------------------

TEST_F(DashboardStateTest, StopClearsRunning)
{
    state_->start();
    std::this_thread::sleep_for(std::chrono::milliseconds{ 100 });

    EXPECT_TRUE(state_->running());

    state_->stop();

    // After stop(), running must be false.
    EXPECT_FALSE(state_->running());
}

// ---------------------------------------------------------------------------
// 4. LatestReturnsNullBeforePoll
// ---------------------------------------------------------------------------

TEST_F(DashboardStateTest, LatestReturnsNullBeforePoll)
{
    // Before start(), latest() must return nullptr.
    const auto snap = state_->latest();
    EXPECT_EQ(nullptr, snap);
}

// ---------------------------------------------------------------------------
// 5. PollProducesSnapshot
// ---------------------------------------------------------------------------

TEST_F(DashboardStateTest, PollProducesSnapshot)
{
    state_->start();

    // Wait for at least one poll cycle (fast tier fires after 200 ms + 50 ms sleep).
    std::this_thread::sleep_for(std::chrono::milliseconds{ 400 });

    const auto snap = state_->latest();
    EXPECT_NE(nullptr, snap);

    state_->stop();
}

// ---------------------------------------------------------------------------
// 6. SnapshotHasTimestamp
// ---------------------------------------------------------------------------

TEST_F(DashboardStateTest, SnapshotHasTimestamp)
{
    state_->start();
    std::this_thread::sleep_for(std::chrono::milliseconds{ 400 });

    const auto snap = state_->latest();
    ASSERT_NE(nullptr, snap);

    // Timestamp must be non-zero (set from system_clock).
    EXPECT_GT(snap->timestamp_ms, 0);

    state_->stop();
}

// ---------------------------------------------------------------------------
// 7. PollFastPopulatesOrderBooks
// ---------------------------------------------------------------------------

TEST_F(DashboardStateTest, PollFastPopulatesOrderBooks)
{
    // Pre-populate the ticker cache with a symbol.
    Ticker ticker;
    ticker.symbol = "BTC_USDT";
    ticker.last = 50000.0;
    ticker.bid = 49999.0;
    ticker.ask = 50001.0;
    ticker.timestamp = 1700000000000;
    market_feed_->ticker_cache().update("BTC_USDT", ticker);

    // Pre-populate the order book with a snapshot.
    nlohmann::json ob_snapshot = {
        {"lastUpdateId", 1},
        {"bids", nlohmann::json::array({
            nlohmann::json::array({50000.0, 1.5}),
            nlohmann::json::array({49999.0, 2.0}),
        })},
        {"asks", nlohmann::json::array({
            nlohmann::json::array({50001.0, 0.8}),
            nlohmann::json::array({50002.0, 1.2}),
        })},
    };
    market_feed_->orderbook_manager().apply_snapshot("BTC_USDT", ob_snapshot);

    state_->start();

    // Wait for the fast tier to fire (200 ms + 50 ms sleep + margin).
    std::this_thread::sleep_for(std::chrono::milliseconds{ 400 });

    const auto snap = state_->latest();
    ASSERT_NE(nullptr, snap);

    // Order book should be populated with the symbol.
    EXPECT_EQ("BTC_USDT", snap->order_book.symbol);
    EXPECT_FALSE(snap->order_book.bids.empty());
    EXPECT_FALSE(snap->order_book.asks.empty());

    state_->stop();
}

// ---------------------------------------------------------------------------
// 8. PollMediumPopulatesPositions
// ---------------------------------------------------------------------------

TEST_F(DashboardStateTest, PollMediumPopulatesPositions)
{
    // Open a position in the position manager.
    auto result = position_mgr_->open_position(
        "BTC_USDT", Side::Buy, 0.001, 50000.0, "test_strategy");
    ASSERT_TRUE(ok(result));

    state_->start();

    // Wait for the medium tier to fire (500 ms + 50 ms sleep + margin).
    std::this_thread::sleep_for(std::chrono::milliseconds{ 800 });

    const auto snap = state_->latest();
    ASSERT_NE(nullptr, snap);

    // Positions should be populated.
    EXPECT_FALSE(snap->positions.positions.empty());
    EXPECT_EQ(1, snap->positions.portfolio.open_position_count);

    state_->stop();
}

// ---------------------------------------------------------------------------
// 9. PollSlowPopulatesStrategies
// ---------------------------------------------------------------------------

TEST_F(DashboardStateTest, PollSlowPopulatesStrategies)
{
    // The strategy manager is empty by default — snapshot should be empty.
    state_->start();

    // Wait for the slow tier to fire (1 s + 50 ms sleep + margin).
    std::this_thread::sleep_for(std::chrono::milliseconds{ 1400 });

    const auto snap = state_->latest();
    ASSERT_NE(nullptr, snap);

    // Strategies panel should be present (even if empty).
    // The slow tier sets snap.strategies.strategies = strategy_mgr_.snapshot()
    // which is empty for an empty manager.
    EXPECT_TRUE(snap->strategies.strategies.empty());

    // Risk panel should also be populated by the slow tier.
    EXPECT_FALSE(snap->risk.trading_halted);

    // Metrics should be marked as unavailable (not implemented yet).
    EXPECT_FALSE(snap->metrics.available);

    state_->stop();
}

// ---------------------------------------------------------------------------
// 10. PollAiPopulatesWhenAvailable
// ---------------------------------------------------------------------------

TEST_F(DashboardStateTest, PollAiPopulatesWhenAvailable)
{
    // The AI pipeline has no result yet (mock transport always fails).
    // The AI snapshot should be unavailable.
    state_->start();

    // Wait for the initial AI poll (fires on first iteration).
    std::this_thread::sleep_for(std::chrono::milliseconds{ 400 });

    const auto snap = state_->latest();
    ASSERT_NE(nullptr, snap);

    // AI should be unavailable (no successful analysis cycle).
    EXPECT_FALSE(snap->ai.available);

    state_->stop();
}

// ---------------------------------------------------------------------------
// 11. SnapshotCallbackInvoked
// ---------------------------------------------------------------------------

TEST_F(DashboardStateTest, SnapshotCallbackInvoked)
{
    std::atomic<int> callback_count{ 0 };

    state_->set_snapshot_callback([&callback_count](std::shared_ptr<const DashboardSnapshot> snap)
    {
        (void)snap;
        callback_count.fetch_add(1, std::memory_order_relaxed);
    });

    state_->start();

    // Wait for multiple poll cycles.
    std::this_thread::sleep_for(std::chrono::milliseconds{ 600 });

    state_->stop();

    // The callback should have been invoked at least once.
    EXPECT_GT(callback_count.load(std::memory_order_relaxed), 0);
}

// ---------------------------------------------------------------------------
// 12. StopTokenCancelsPollLoop
// ---------------------------------------------------------------------------

TEST_F(DashboardStateTest, StopTokenCancelsPollLoop)
{
    state_->start();
    std::this_thread::sleep_for(std::chrono::milliseconds{ 100 });

    EXPECT_TRUE(state_->running());

    // Stop should cause the poll loop to exit within a reasonable time.
    state_->stop();

    // After stop(), running must be false within 200 ms.
    EXPECT_FALSE(state_->running());
}
