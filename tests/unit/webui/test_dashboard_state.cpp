// test_dashboard_state.cpp — Unit tests for DashboardState polling aggregator (Layer 9 WebUI)
//
// Tests the tiered polling loop that assembles DashboardSnapshot from upstream
// components. Uses real component instances with empty/mock configurations.
//
// Component construction approach:
//   - GateWsClient / GateRestClient: constructed with empty ExchangeConfig (never started)
//   - MarketFeed: constructed with above clients (never started); tickerCache and
//     orderbookManager are populated directly for tests
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

#include "webui/DashboardState.hpp"

#include "ai/AiPipeline.hpp"
#include "core/config.hpp"
#include "exchange/GateRestClient.hpp"
#include "exchange/GateWsClient.hpp"
#include "execution/OrderTracker.hpp"
#include "market/MarketFeed.hpp"
#include "risk/DrawdownGuard.hpp"
#include "risk/OrderRateLimiter.hpp"
#include "risk/PositionManager.hpp"
#include "risk/RiskManager.hpp"
#include "strategy/StrategyManager.hpp"

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
        m_wsClient = std::make_unique<GateWsClient>(exchange_config);
        m_restClient = std::make_unique<GateRestClient>(exchange_config);

        // 2. MarketFeed (not started — components populated directly).
        m_marketFeed = std::make_unique<MarketFeed>(*m_wsClient, *m_restClient);

        // 3. StrategyManager (empty — strategies registered per-test).
        m_strategyMgr = std::make_unique<StrategyManager>();

        // 4. Risk components.
        risk_config_ = RiskConfig{};
        position_mgr_ = std::make_unique<PositionManager>(risk_config_);
        m_drawdownGuard = std::make_unique<DrawdownGuard>(risk_config_);
        m_rateLimiter = std::make_unique<OrderRateLimiter>(risk_config_.maxOrdersPerSec);
        m_riskMgr = std::make_unique<RiskManager>(
            risk_config_, *position_mgr_, *m_drawdownGuard, *m_rateLimiter);

        // 5. OrderTracker with empty WS/REST clients.
        m_orderTracker = std::make_unique<OrderTracker>(*m_wsClient, *m_restClient);

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

        m_aiPipeline = std::make_unique<AiPipeline>(
            ai_config, tw_config, news_config, mock_transport);

        // 7. WebUI config.
        webui_config_ = WebUiConfig{};

        // 8. DashboardState under test.
        m_state = std::make_unique<DashboardState>(
            webui_config_,
            *m_marketFeed,
            *m_strategyMgr,
            *m_riskMgr,
            *m_orderTracker,
            *m_aiPipeline);
    }

    void TearDown() override
    {
        // Ensure the state is stopped before tearing down.
        if (m_state)
        {
            m_state->stop();
        }
    }

    // Components (owned by fixture).
    std::unique_ptr<GateWsClient> m_wsClient;
    std::unique_ptr<GateRestClient> m_restClient;
    std::unique_ptr<MarketFeed> m_marketFeed;
    std::unique_ptr<StrategyManager> m_strategyMgr;
    RiskConfig risk_config_;
    std::unique_ptr<PositionManager> position_mgr_;
    std::unique_ptr<DrawdownGuard> m_drawdownGuard;
    std::unique_ptr<OrderRateLimiter> m_rateLimiter;
    std::unique_ptr<RiskManager> m_riskMgr;
    std::unique_ptr<OrderTracker> m_orderTracker;
    std::unique_ptr<AiPipeline> m_aiPipeline;
    WebUiConfig webui_config_;
    std::unique_ptr<DashboardState> m_state;
};

// ---------------------------------------------------------------------------
// 1. DefaultStateIsNotRunning
// ---------------------------------------------------------------------------

TEST_F(DashboardStateTest, DefaultStateIsNotRunning)
{
    // A freshly constructed DashboardState must not be running.
    EXPECT_FALSE(m_state->running());
}

// ---------------------------------------------------------------------------
// 2. StartSetsRunning
// ---------------------------------------------------------------------------

TEST_F(DashboardStateTest, StartSetsRunning)
{
    m_state->start();

    // Give the thread a moment to start.
    std::this_thread::sleep_for(std::chrono::milliseconds{ 100 });

    EXPECT_TRUE(m_state->running());

    m_state->stop();
}

// ---------------------------------------------------------------------------
// 3. StopClearsRunning
// ---------------------------------------------------------------------------

TEST_F(DashboardStateTest, StopClearsRunning)
{
    m_state->start();
    std::this_thread::sleep_for(std::chrono::milliseconds{ 100 });

    EXPECT_TRUE(m_state->running());

    m_state->stop();

    // After stop(), running must be false.
    EXPECT_FALSE(m_state->running());
}

// ---------------------------------------------------------------------------
// 4. LatestReturnsNullBeforePoll
// ---------------------------------------------------------------------------

TEST_F(DashboardStateTest, LatestReturnsNullBeforePoll)
{
    // Before start(), latest() must return nullptr.
    const auto snap = m_state->latest();
    EXPECT_EQ(nullptr, snap);
}

// ---------------------------------------------------------------------------
// 5. PollProducesSnapshot
// ---------------------------------------------------------------------------

TEST_F(DashboardStateTest, PollProducesSnapshot)
{
    m_state->start();

    // Wait for at least one poll cycle (fast tier fires after 200 ms + 50 ms sleep).
    std::this_thread::sleep_for(std::chrono::milliseconds{ 400 });

    const auto snap = m_state->latest();
    EXPECT_NE(nullptr, snap);

    m_state->stop();
}

// ---------------------------------------------------------------------------
// 6. SnapshotHasTimestamp
// ---------------------------------------------------------------------------

TEST_F(DashboardStateTest, SnapshotHasTimestamp)
{
    m_state->start();
    std::this_thread::sleep_for(std::chrono::milliseconds{ 400 });

    const auto snap = m_state->latest();
    ASSERT_NE(nullptr, snap);

    // Timestamp must be non-zero (set from system_clock).
    EXPECT_GT(snap->timestamp_ms, 0);

    m_state->stop();
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
    m_marketFeed->tickerCache().update("BTC_USDT", ticker);

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
    m_marketFeed->orderbookManager().applySnapshot("BTC_USDT", ob_snapshot);

    m_state->start();

    // Wait for the fast tier to fire (200 ms + 50 ms sleep + margin).
    std::this_thread::sleep_for(std::chrono::milliseconds{ 400 });

    const auto snap = m_state->latest();
    ASSERT_NE(nullptr, snap);

    // Order book should be populated with the symbol.
    EXPECT_EQ("BTC_USDT", snap->order_book.symbol);
    EXPECT_FALSE(snap->order_book.bids.empty());
    EXPECT_FALSE(snap->order_book.asks.empty());

    m_state->stop();
}

// ---------------------------------------------------------------------------
// 8. PollMediumPopulatesPositions
// ---------------------------------------------------------------------------

TEST_F(DashboardStateTest, PollMediumPopulatesPositions)
{
    // Open a position in the position manager.
    auto result = position_mgr_->openPosition(
        "BTC_USDT", Side::Buy, 0.001, 50000.0, "test_strategy");
    ASSERT_TRUE(ok(result));

    m_state->start();

    // Wait for the medium tier to fire (500 ms + 50 ms sleep + margin).
    std::this_thread::sleep_for(std::chrono::milliseconds{ 800 });

    const auto snap = m_state->latest();
    ASSERT_NE(nullptr, snap);

    // Positions should be populated.
    EXPECT_FALSE(snap->positions.positions.empty());
    EXPECT_EQ(1, snap->positions.portfolio.openPositionCount);

    m_state->stop();
}

// ---------------------------------------------------------------------------
// 9. PollSlowPopulatesStrategies
// ---------------------------------------------------------------------------

TEST_F(DashboardStateTest, PollSlowPopulatesStrategies)
{
    // The strategy manager is empty by default — snapshot should be empty.
    m_state->start();

    // Wait for the slow tier to fire (1 s + 50 ms sleep + margin).
    std::this_thread::sleep_for(std::chrono::milliseconds{ 1400 });

    const auto snap = m_state->latest();
    ASSERT_NE(nullptr, snap);

    // Strategies panel should be present (even if empty).
    // The slow tier sets snap.strategies.strategies = m_strategyMgr.snapshot()
    // which is empty for an empty manager.
    EXPECT_TRUE(snap->strategies.strategies.empty());

    // Risk panel should also be populated by the slow tier.
    EXPECT_FALSE(snap->risk.trading_halted);

    // Metrics should be marked as unavailable (not implemented yet).
    EXPECT_FALSE(snap->metrics.available);

    m_state->stop();
}

// ---------------------------------------------------------------------------
// 10. PollAiPopulatesWhenAvailable
// ---------------------------------------------------------------------------

TEST_F(DashboardStateTest, PollAiPopulatesWhenAvailable)
{
    // The AI pipeline has no result yet (mock transport always fails).
    // The AI snapshot should be unavailable.
    m_state->start();

    // Wait for the initial AI poll (fires on first iteration).
    std::this_thread::sleep_for(std::chrono::milliseconds{ 400 });

    const auto snap = m_state->latest();
    ASSERT_NE(nullptr, snap);

    // AI should be unavailable (no successful analysis cycle).
    EXPECT_FALSE(snap->ai.available);

    m_state->stop();
}

// ---------------------------------------------------------------------------
// 11. SnapshotCallbackInvoked
// ---------------------------------------------------------------------------

TEST_F(DashboardStateTest, SnapshotCallbackInvoked)
{
    std::atomic<int> callback_count{ 0 };

    m_state->setSnapshotCallback([&callback_count](std::shared_ptr<const DashboardSnapshot> snap)
    {
        (void)snap;
        callback_count.fetch_add(1, std::memory_order_relaxed);
    });

    m_state->start();

    // Wait for multiple poll cycles.
    std::this_thread::sleep_for(std::chrono::milliseconds{ 600 });

    m_state->stop();

    // The callback should have been invoked at least once.
    EXPECT_GT(callback_count.load(std::memory_order_relaxed), 0);
}

// ---------------------------------------------------------------------------
// 12. StopTokenCancelsPollLoop
// ---------------------------------------------------------------------------

TEST_F(DashboardStateTest, StopTokenCancelsPollLoop)
{
    m_state->start();
    std::this_thread::sleep_for(std::chrono::milliseconds{ 100 });

    EXPECT_TRUE(m_state->running());

    // Stop should cause the poll loop to exit within a reasonable time.
    m_state->stop();

    // After stop(), running must be false within 200 ms.
    EXPECT_FALSE(m_state->running());
}
