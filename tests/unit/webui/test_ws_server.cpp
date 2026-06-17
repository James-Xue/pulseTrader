// test_ws_server.cpp — Unit tests for WsServer WebSocket push helper (Layer 9 WebUI)
//
// Tests the WsServer component in isolation without requiring a real uWebSockets
// server. The publish function is mocked to verify it's called correctly.
//
// Tests:
//   1. DefaultClientCountIsZero     — client_count() is 0 on construction
//   2. MaxClientsMatchesConfig      — max_clients() reflects config.maxClients
//   3. PushSnapshotNoClientsSafe    — push_snapshot() with no clients doesn't crash
//   4. OnWsOpenIncrementsCount      — on_ws_open() increments client_count()
//   5. OnWsCloseDecrementsCount     — on_ws_close() decrements client_count()
//   6. OnWsCloseDoesNotUnderflow    — on_ws_close() at 0 stays at 0
//   7. PushSnapshotCachesJson       — push_snapshot() caches serialized JSON
//   8. PushSnapshotCallsPublishFn   — publish function invoked when clients > 0

#include "pulse/webui/ws_server.hpp"

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
        config_.enabled = true;
        config_.bindAddress = "127.0.0.1";
        config_.port = 8080;
        config_.authToken = "test-token";
        config_.maxClients = 4;

        ws_server_ = std::make_unique<WsServer>(config_);
    }

    void TearDown() override
    {
        ws_server_.reset();
    }

    WebUiConfig config_;
    std::unique_ptr<WsServer> ws_server_;
};

// ---------------------------------------------------------------------------
// 1. DefaultClientCountIsZero — client_count() is 0 on construction
// ---------------------------------------------------------------------------

TEST_F(WsServerTest, DefaultClientCountIsZero)
{
    // A freshly constructed WsServer must have zero connected clients.
    EXPECT_EQ(0u, ws_server_->client_count());
}

// ---------------------------------------------------------------------------
// 2. MaxClientsMatchesConfig — max_clients() reflects config.maxClients
// ---------------------------------------------------------------------------

TEST_F(WsServerTest, MaxClientsMatchesConfig)
{
    EXPECT_EQ(4u, ws_server_->max_clients());

    // Test with a different config.
    WebUiConfig config2;
    config2.maxClients = 16;
    WsServer ws2{ config2 };
    EXPECT_EQ(16u, ws2.max_clients());
}

// ---------------------------------------------------------------------------
// 3. PushSnapshotNoClientsSafe — push_snapshot() with no clients doesn't crash
// ---------------------------------------------------------------------------

TEST_F(WsServerTest, PushSnapshotNoClientsSafe)
{
    // Create a minimal snapshot.
    auto snap = std::make_shared<const DashboardSnapshot>();

    // Push with zero clients and no publish function set — must not crash.
    EXPECT_NO_THROW(ws_server_->push_snapshot(snap));

    // JSON should still be cached even with no clients.
    EXPECT_FALSE(ws_server_->last_json().empty());
}

// ---------------------------------------------------------------------------
// 4. OnWsOpenIncrementsCount — on_ws_open() increments client_count()
// ---------------------------------------------------------------------------

TEST_F(WsServerTest, OnWsOpenIncrementsCount)
{
    ws_server_->on_ws_open();
    EXPECT_EQ(1u, ws_server_->client_count());

    ws_server_->on_ws_open();
    EXPECT_EQ(2u, ws_server_->client_count());

    ws_server_->on_ws_open();
    EXPECT_EQ(3u, ws_server_->client_count());
}

// ---------------------------------------------------------------------------
// 5. OnWsCloseDecrementsCount — on_ws_close() decrements client_count()
// ---------------------------------------------------------------------------

TEST_F(WsServerTest, OnWsCloseDecrementsCount)
{
    // Open 3 clients, then close 2.
    ws_server_->on_ws_open();
    ws_server_->on_ws_open();
    ws_server_->on_ws_open();
    ASSERT_EQ(3u, ws_server_->client_count());

    ws_server_->on_ws_close();
    EXPECT_EQ(2u, ws_server_->client_count());

    ws_server_->on_ws_close();
    EXPECT_EQ(1u, ws_server_->client_count());
}

// ---------------------------------------------------------------------------
// 6. OnWsCloseDoesNotUnderflow — on_ws_close() at 0 stays at 0
// ---------------------------------------------------------------------------

TEST_F(WsServerTest, OnWsCloseDoesNotUnderflow)
{
    // Close without any opens — should stay at 0 (not underflow).
    ws_server_->on_ws_close();
    EXPECT_EQ(0u, ws_server_->client_count());

    // Multiple closes should still be safe.
    ws_server_->on_ws_close();
    ws_server_->on_ws_close();
    EXPECT_EQ(0u, ws_server_->client_count());
}

// ---------------------------------------------------------------------------
// 7. PushSnapshotCachesJson — push_snapshot() caches serialized JSON
// ---------------------------------------------------------------------------

TEST_F(WsServerTest, PushSnapshotCachesJson)
{
    // Initially, last_json() should be empty.
    EXPECT_TRUE(ws_server_->last_json().empty());

    // Create a snapshot with a known timestamp.
    auto snap = std::make_shared<DashboardSnapshot>();
    // Note: snap is non-const here for modification, but push_snapshot takes
    // shared_ptr<const DashboardSnapshot>. The implicit conversion handles this.
    snap->timestamp_ms = 1700000000000;

    ws_server_->push_snapshot(snap);

    // JSON should now be cached.
    const auto json = ws_server_->last_json();
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

    ws_server_->set_publish_fn([&](const std::string &json)
    {
        publish_count.fetch_add(1, std::memory_order_relaxed);
        last_published_json = json;
    });

    // With zero clients, the publish function should NOT be called.
    auto snap = std::make_shared<const DashboardSnapshot>();
    ws_server_->push_snapshot(snap);
    EXPECT_EQ(0, publish_count.load(std::memory_order_relaxed));

    // Simulate a client connecting.
    ws_server_->on_ws_open();
    ASSERT_EQ(1u, ws_server_->client_count());

    // Now push_snapshot should invoke the publish function.
    ws_server_->push_snapshot(snap);
    EXPECT_EQ(1, publish_count.load(std::memory_order_relaxed));
    EXPECT_FALSE(last_published_json.empty());

    // Push again — should invoke again.
    ws_server_->push_snapshot(snap);
    EXPECT_EQ(2, publish_count.load(std::memory_order_relaxed));

    // Close the client, push again — should NOT invoke.
    ws_server_->on_ws_close();
    ASSERT_EQ(0u, ws_server_->client_count());

    ws_server_->push_snapshot(snap);
    EXPECT_EQ(2, publish_count.load(std::memory_order_relaxed)); // Still 2
}
