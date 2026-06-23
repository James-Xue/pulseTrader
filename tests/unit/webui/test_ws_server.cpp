// test_ws_server.cpp — Unit tests for WsServer WebSocket push helper (Layer 9 WebUI)
//
// Tests the WsServer component in isolation without requiring a real uWebSockets
// server. The publish function is mocked to verify it's called correctly.
//
// Tests:
//   1. DefaultClientCountIsZero     — clientCount() is 0 on construction
//   2. MaxClientsMatchesConfig      — maxClients() reflects config.maxClients
//   3. PushSnapshotNoClientsSafe    — pushSnapshot() with no clients doesn't crash
//   4. OnWsOpenIncrementsCount      — onWsOpen() increments clientCount()
//   5. OnWsCloseDecrementsCount     — onWsClose() decrements clientCount()
//   6. OnWsCloseDoesNotUnderflow    — onWsClose() at 0 stays at 0
//   7. PushSnapshotCachesJson       — pushSnapshot() caches serialized JSON
//   8. PushSnapshotCallsPublishFn   — publish function invoked when clients > 0

#include "webui/ws_server.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

using namespace pulse;
using namespace pulse::webui;

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class WsServerTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_config.enabled = true;
        m_config.bindAddress = "127.0.0.1";
        m_config.port = 8080;
        m_config.authToken = "test-token";
        m_config.maxClients = 4;

        m_wsServer = std::make_unique<WsServer>(m_config);
    }

    void TearDown() override
    {
        m_wsServer.reset();
    }

    WebUiConfig m_config;
    std::unique_ptr<WsServer> m_wsServer;
};

// ---------------------------------------------------------------------------
// 1. DefaultClientCountIsZero — clientCount() is 0 on construction
// ---------------------------------------------------------------------------

TEST_F(WsServerTest, DefaultClientCountIsZero)
{
    // A freshly constructed WsServer must have zero connected clients.
    EXPECT_EQ(0u, m_wsServer->clientCount());
}

// ---------------------------------------------------------------------------
// 2. MaxClientsMatchesConfig — maxClients() reflects config.maxClients
// ---------------------------------------------------------------------------

TEST_F(WsServerTest, MaxClientsMatchesConfig)
{
    EXPECT_EQ(4u, m_wsServer->maxClients());

    // Test with a different config.
    WebUiConfig config2;
    config2.maxClients = 16;
    WsServer ws2{ config2 };
    EXPECT_EQ(16u, ws2.maxClients());
}

// ---------------------------------------------------------------------------
// 3. PushSnapshotNoClientsSafe — pushSnapshot() with no clients doesn't crash
// ---------------------------------------------------------------------------

TEST_F(WsServerTest, PushSnapshotNoClientsSafe)
{
    // Create a minimal snapshot.
    auto snap = std::make_shared<const DashboardSnapshot>();

    // Push with zero clients and no publish function set — must not crash.
    EXPECT_NO_THROW(m_wsServer->pushSnapshot(snap));

    // JSON should still be cached even with no clients.
    EXPECT_FALSE(m_wsServer->lastJson().empty());
}

// ---------------------------------------------------------------------------
// 4. OnWsOpenIncrementsCount — onWsOpen() increments clientCount()
// ---------------------------------------------------------------------------

TEST_F(WsServerTest, OnWsOpenIncrementsCount)
{
    m_wsServer->onWsOpen();
    EXPECT_EQ(1u, m_wsServer->clientCount());

    m_wsServer->onWsOpen();
    EXPECT_EQ(2u, m_wsServer->clientCount());

    m_wsServer->onWsOpen();
    EXPECT_EQ(3u, m_wsServer->clientCount());
}

// ---------------------------------------------------------------------------
// 5. OnWsCloseDecrementsCount — onWsClose() decrements clientCount()
// ---------------------------------------------------------------------------

TEST_F(WsServerTest, OnWsCloseDecrementsCount)
{
    // Open 3 clients, then close 2.
    m_wsServer->onWsOpen();
    m_wsServer->onWsOpen();
    m_wsServer->onWsOpen();
    ASSERT_EQ(3u, m_wsServer->clientCount());

    m_wsServer->onWsClose();
    EXPECT_EQ(2u, m_wsServer->clientCount());

    m_wsServer->onWsClose();
    EXPECT_EQ(1u, m_wsServer->clientCount());
}

// ---------------------------------------------------------------------------
// 6. OnWsCloseDoesNotUnderflow — onWsClose() at 0 stays at 0
// ---------------------------------------------------------------------------

TEST_F(WsServerTest, OnWsCloseDoesNotUnderflow)
{
    // Close without any opens — should stay at 0 (not underflow).
    m_wsServer->onWsClose();
    EXPECT_EQ(0u, m_wsServer->clientCount());

    // Multiple closes should still be safe.
    m_wsServer->onWsClose();
    m_wsServer->onWsClose();
    EXPECT_EQ(0u, m_wsServer->clientCount());
}

// ---------------------------------------------------------------------------
// 7. PushSnapshotCachesJson — pushSnapshot() caches serialized JSON
// ---------------------------------------------------------------------------

TEST_F(WsServerTest, PushSnapshotCachesJson)
{
    // Initially, lastJson() should be empty.
    EXPECT_TRUE(m_wsServer->lastJson().empty());

    // Create a snapshot with a known timestamp.
    auto snap = std::make_shared<DashboardSnapshot>();
    // Note: snap is non-const here for modification, but pushSnapshot takes
    // shared_ptr<const DashboardSnapshot>. The implicit conversion handles this.
    snap->timestamp_ms = 1700000000000;

    m_wsServer->pushSnapshot(snap);

    // JSON should now be cached.
    const auto json = m_wsServer->lastJson();
    EXPECT_FALSE(json.empty());

    // Verify it's valid JSON with the expected timestamp.
    const auto j = nlohmann::json::parse(json, nullptr, false);
    ASSERT_FALSE(j.is_discarded()) << "Cached JSON is not valid: " << json;
    EXPECT_EQ(INT64_C(1700000000000), j["timestamp_ms"].get<std::int64_t>());
}

// ---------------------------------------------------------------------------
// 8. PushSnapshotCallsPublishFn — publish function invoked when clients > 0
// ---------------------------------------------------------------------------

TEST_F(WsServerTest, PushSnapshotCallsPublishFn)
{
    // Track publish function invocations.
    std::atomic<int> publish_count{ 0 };
    std::string last_published_json;

    m_wsServer->setPublishFn([&](const std::string &json)
    {
        publish_count.fetch_add(1, std::memory_order_relaxed);
        last_published_json = json;
    });

    // With zero clients, the publish function should NOT be called.
    auto snap = std::make_shared<const DashboardSnapshot>();
    m_wsServer->pushSnapshot(snap);
    EXPECT_EQ(0, publish_count.load(std::memory_order_relaxed));

    // Simulate a client connecting.
    m_wsServer->onWsOpen();
    ASSERT_EQ(1u, m_wsServer->clientCount());

    // Now pushSnapshot should invoke the publish function.
    m_wsServer->pushSnapshot(snap);
    EXPECT_EQ(1, publish_count.load(std::memory_order_relaxed));
    EXPECT_FALSE(last_published_json.empty());

    // Push again — should invoke again.
    m_wsServer->pushSnapshot(snap);
    EXPECT_EQ(2, publish_count.load(std::memory_order_relaxed));

    // Close the client, push again — should NOT invoke.
    m_wsServer->onWsClose();
    ASSERT_EQ(0u, m_wsServer->clientCount());

    m_wsServer->pushSnapshot(snap);
    EXPECT_EQ(2, publish_count.load(std::memory_order_relaxed)); // Still 2
}
