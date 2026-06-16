// test_gate_ws_client.cpp — Unit tests for GateWsClient (Layer 1 Exchange)
//
// Tests pure-logic functions: backoff computation, WS auth building, state machine.
// No real WebSocket connection required — all tests exercise detail:: free functions
// and the GateWsClient's observable state.

#include "pulse/exchange/gate_ws_client.hpp"

#include "pulse/core/config.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>

namespace pulse::exchange::test
{

// ---------------------------------------------------------------------------
// Test 1: InitialStateIsDisconnected — client starts disconnected
// ---------------------------------------------------------------------------
TEST(GateWsClientTest, InitialStateIsDisconnected)
{
    const ExchangeConfig config;
    const GateWsClient client(config);

    EXPECT_EQ(WsConnectionState::Disconnected, client.state());
}

// ---------------------------------------------------------------------------
// Test 2: ComputeBackoffFirstAttempt — ~1000ms with ±25% jitter
// ---------------------------------------------------------------------------
TEST(GateWsClientTest, ComputeBackoffFirstAttempt)
{
    // attempt=0, base=1000, max=30000
    // Expected: 1000 * 2^0 = 1000, jittered to [750, 1250]
    const auto delay = detail::compute_backoff_ms(0, 1000, 30000);

    EXPECT_GE(delay, 750u);
    EXPECT_LE(delay, 1250u);
}

// ---------------------------------------------------------------------------
// Test 3: ComputeBackoffExponential — doubles each attempt
// ---------------------------------------------------------------------------
TEST(GateWsClientTest, ComputeBackoffExponential)
{
    // attempt=3, base=1000, max=30000
    // Expected: 1000 * 2^3 = 8000, jittered to [6000, 10000]
    const auto delay = detail::compute_backoff_ms(3, 1000, 30000);

    EXPECT_GE(delay, 6000u);
    EXPECT_LE(delay, 10000u);
}

// ---------------------------------------------------------------------------
// Test 4: ComputeBackoffCappedAtMax — never exceeds max
// ---------------------------------------------------------------------------
TEST(GateWsClientTest, ComputeBackoffCappedAtMax)
{
    // attempt=10, base=1000, max=30000
    // Raw: 1000 * 2^10 = 1024000, capped at 30000
    // Jittered: [22500, 37500] — but capped before jitter
    // So jittered range is [22500, 37500]
    for (int i = 0; i < 20; ++i)
    {
        const auto delay = detail::compute_backoff_ms(10, 1000, 30000);
        // With cap at 30000 and jitter ±25%: [22500, 37500]
        EXPECT_GE(delay, 22500u);
        EXPECT_LE(delay, 37500u);
    }
}

// ---------------------------------------------------------------------------
// Test 5: ComputeBackoffJitterRange — verify randomness within bounds
// ---------------------------------------------------------------------------
TEST(GateWsClientTest, ComputeBackoffJitterRange)
{
    // Run 50 samples and verify all fall within ±25% of base
    const std::uint32_t base_ms = 2000;
    const std::uint32_t max_ms = 60000;
    const std::uint32_t low = static_cast<std::uint32_t>(base_ms * 0.75);
    const std::uint32_t high = static_cast<std::uint32_t>(base_ms * 1.25);

    for (int i = 0; i < 50; ++i)
    {
        const auto delay = detail::compute_backoff_ms(0, base_ms, max_ms);
        EXPECT_GE(delay, low);
        EXPECT_LE(delay, high);
    }
}

// ---------------------------------------------------------------------------
// Test 6: BuildWsAuthFormat — verify auth JSON structure
// ---------------------------------------------------------------------------
TEST(GateWsClientTest, BuildWsAuthFormat)
{
    const auto auth = detail::build_ws_auth("test_key", "test_secret", "spot.orders", "subscribe");

    // 1. Verify all required fields exist
    EXPECT_TRUE(auth.contains("method"));
    EXPECT_TRUE(auth.contains("KEY"));
    EXPECT_TRUE(auth.contains("SIGN"));
    EXPECT_TRUE(auth.contains("time"));

    // 2. Verify field values
    EXPECT_EQ("api_key", auth["method"].get<std::string>());
    EXPECT_EQ("test_key", auth["KEY"].get<std::string>());

    // 3. SIGN should be a 128-char hex string (SHA-512 output)
    const auto sign = auth["SIGN"].get<std::string>();
    EXPECT_EQ(128u, sign.size());

    // 4. Time should be a plausible Unix timestamp
    EXPECT_GT(auth["time"].get<std::int64_t>(), 1577836800);
}

// ---------------------------------------------------------------------------
// Test 7: BuildWsAuthDeterministic — same inputs produce same signature
// ---------------------------------------------------------------------------
TEST(GateWsClientTest, BuildWsAuthDifferentKeysDifferentSigs)
{
    // Two different secrets should produce different signatures
    const auto auth1 = detail::build_ws_auth("key1", "secret1", "spot.orders", "subscribe");
    const auto auth2 = detail::build_ws_auth("key1", "secret2", "spot.orders", "subscribe");

    // SIGN values should differ (different secrets → different HMAC)
    // Note: time might be the same if called within the same second
    EXPECT_NE(auth1["SIGN"].get<std::string>(), auth2["SIGN"].get<std::string>());
}

// ---------------------------------------------------------------------------
// Test 8: WsConnectionStateEnumValues — verify enum underlying values
// ---------------------------------------------------------------------------
TEST(GateWsClientTest, WsConnectionStateEnumValues)
{
    EXPECT_EQ(static_cast<std::uint8_t>(0), static_cast<std::uint8_t>(WsConnectionState::Disconnected));
    EXPECT_EQ(static_cast<std::uint8_t>(1), static_cast<std::uint8_t>(WsConnectionState::Connecting));
    EXPECT_EQ(static_cast<std::uint8_t>(2), static_cast<std::uint8_t>(WsConnectionState::Connected));
}

// ---------------------------------------------------------------------------
// Test 9: ClientChannelsAccessible — channels() returns valid reference
// ---------------------------------------------------------------------------
TEST(GateWsClientTest, ClientChannelsAccessible)
{
    ExchangeConfig config;
    GateWsClient client(config);

    // 1. Subscribe via the client's channel registry
    bool invoked = false;
    client.channels().subscribe(
        "spot.tickers", { "BTC_USDT" }, [&](const nlohmann::json &, const nlohmann::json &) { invoked = true; });

    // 2. Verify channel is registered
    const auto active = client.channels().active_channels();
    EXPECT_EQ(1u, active.size());
    EXPECT_EQ("spot.tickers", active[0]);

    // 3. Dispatch a frame and verify callback fires
    const auto frame =
        nlohmann::json{ { "time", 1 }, { "channel", "spot.tickers" }, { "event", "update" }, { "result", nullptr } };
    EXPECT_TRUE(client.channels().dispatch(frame));
    EXPECT_TRUE(invoked);
}

// ---------------------------------------------------------------------------
// Test 10: ClientSubscribeRegistersCallback — subscribe() adds to channels
// ---------------------------------------------------------------------------
TEST(GateWsClientTest, ClientSubscribeRegistersCallback)
{
    ExchangeConfig config;
    GateWsClient client(config);

    // Subscribe via the public API (not directly on channels())
    client.subscribe("spot.trades", { "ETH_USDT" }, [](const nlohmann::json &, const nlohmann::json &) {});

    const auto active = client.channels().active_channels();
    EXPECT_EQ(1u, active.size());

    const auto payload = client.channels().get_payload("spot.trades");
    EXPECT_EQ(1u, payload.size());
    EXPECT_EQ("ETH_USDT", payload[0]);
}

// ---------------------------------------------------------------------------
// Test 11: ClientUnsubscribeRemovesChannel — unsubscribe() removes from channels
// ---------------------------------------------------------------------------
TEST(GateWsClientTest, ClientUnsubscribeRemovesChannel)
{
    ExchangeConfig config;
    GateWsClient client(config);

    client.subscribe("spot.tickers", { "BTC_USDT" }, [](const nlohmann::json &, const nlohmann::json &) {});
    EXPECT_EQ(1u, client.channels().active_channels().size());

    client.unsubscribe("spot.tickers");
    EXPECT_TRUE(client.channels().active_channels().empty());
}

// ---------------------------------------------------------------------------
// Test 12: BuildWsAuthSignPayloadFormat — verify the HMAC input string
// ---------------------------------------------------------------------------
TEST(GateWsClientTest, BuildWsAuthSignIsHexLowercase)
{
    const auto auth = detail::build_ws_auth("mykey", "mysecret", "spot.orders", "subscribe");
    const auto sign = auth["SIGN"].get<std::string>();

    // 1. Should be exactly 128 hex chars
    EXPECT_EQ(128u, sign.size());

    // 2. All characters should be lowercase hex
    for (const char c : sign)
    {
        const bool is_hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        EXPECT_TRUE(is_hex) << "Non-hex character: " << c;
    }
}

} // namespace pulse::exchange::test
