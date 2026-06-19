// test_gate_ws_channels.cpp — Unit tests for GateWsChannels (Layer 1 Exchange)
//
// Tests the channel subscription registry, frame dispatch, and message builders.
// No WebSocket connection required — pure logic tests.

#include "exchange/gate_ws_channels.hpp"

#include "core/types.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <string>
#include <vector>

namespace pulse::exchange::test
{

// ---------------------------------------------------------------------------
// Test 1: SubscribeAndDispatch — register callback, dispatch matching frame
// ---------------------------------------------------------------------------
TEST(GateWsChannelsTest, SubscribeAndDispatch)
{
    GateWsChannels channels;
    std::atomic<int> invoke_count{ 0 };
    nlohmann::json captured_result;

    // 1. Subscribe to spot.tickers with a callback that records invocations
    channels.subscribe("spot.tickers",
        { "BTC_USDT" },
        [&](const nlohmann::json &result, const nlohmann::json & /*frame*/)
        {
            captured_result = result;
            invoke_count.fetch_add(1);
        });

    // 2. Dispatch a matching frame
    const auto frame = nlohmann::json{ { "time", 1700000000 },
        { "channel", "spot.tickers" },
        { "event", "update" },
        { "result", { { "currency_pair", "BTC_USDT" }, { "last", "65000.00" } } } };

    const bool dispatched = channels.dispatch(frame);

    // 3. Verify callback was invoked with correct result
    EXPECT_TRUE(dispatched);
    EXPECT_EQ(1, invoke_count.load());
    EXPECT_EQ("BTC_USDT", captured_result["currency_pair"].get<std::string>());
    EXPECT_EQ("65000.00", captured_result["last"].get<std::string>());
}

// ---------------------------------------------------------------------------
// Test 2: DispatchNoHandler — dispatch frame for unknown channel
// ---------------------------------------------------------------------------
TEST(GateWsChannelsTest, DispatchNoHandler)
{
    GateWsChannels channels;

    // 1. Dispatch without any subscription
    const auto frame = nlohmann::json{
        { "time", 1700000000 }, { "channel", "spot.trades" }, { "event", "update" }, { "result", nullptr }
    };

    const bool dispatched = channels.dispatch(frame);

    // 2. Should return false — no handler registered
    EXPECT_FALSE(dispatched);
}

// ---------------------------------------------------------------------------
// Test 3: DispatchMissingChannelField — frame without "channel" field
// ---------------------------------------------------------------------------
TEST(GateWsChannelsTest, DispatchMissingChannelField)
{
    GateWsChannels channels;

    // 1. Subscribe to a channel
    channels.subscribe("spot.tickers", { "BTC_USDT" }, [](const nlohmann::json &, const nlohmann::json &) {});

    // 2. Dispatch a frame missing the "channel" field
    const auto frame = nlohmann::json{ { "time", 1700000000 }, { "event", "update" } };

    const bool dispatched = channels.dispatch(frame);

    // 3. Should return false — cannot route without channel name
    EXPECT_FALSE(dispatched);
}

// ---------------------------------------------------------------------------
// Test 4: UnsubscribeRemovesCallback — unsubscribe, then dispatch returns false
// ---------------------------------------------------------------------------
TEST(GateWsChannelsTest, UnsubscribeRemovesCallback)
{
    GateWsChannels channels;
    std::atomic<int> invoke_count{ 0 };

    // 1. Subscribe
    channels.subscribe("spot.tickers",
        { "BTC_USDT" },
        [&](const nlohmann::json &, const nlohmann::json &) { invoke_count.fetch_add(1); });

    // 2. Verify dispatch works
    const auto frame =
        nlohmann::json{ { "time", 1 }, { "channel", "spot.tickers" }, { "event", "update" }, { "result", nullptr } };
    EXPECT_TRUE(channels.dispatch(frame));
    EXPECT_EQ(1, invoke_count.load());

    // 3. Unsubscribe
    channels.unsubscribe("spot.tickers");

    // 4. Dispatch should now fail
    EXPECT_FALSE(channels.dispatch(frame));
    EXPECT_EQ(1, invoke_count.load()); // callback not invoked again
}

// ---------------------------------------------------------------------------
// Test 5: MultipleChannels — two channels, dispatch routes correctly
// ---------------------------------------------------------------------------
TEST(GateWsChannelsTest, MultipleChannels)
{
    GateWsChannels channels;
    std::string last_ticker;
    std::string last_trade;

    // 1. Subscribe to two different channels
    channels.subscribe("spot.tickers",
        { "BTC_USDT" },
        [&](const nlohmann::json &result, const nlohmann::json &) { last_ticker = result.value("currency_pair", ""); });

    channels.subscribe("spot.trades",
        { "BTC_USDT" },
        [&](const nlohmann::json &result, const nlohmann::json &) { last_trade = result.value("id", ""); });

    // 2. Dispatch a ticker frame
    const auto ticker_frame = nlohmann::json{ { "time", 1 },
        { "channel", "spot.tickers" },
        { "event", "update" },
        { "result", { { "currency_pair", "BTC_USDT" } } } };
    EXPECT_TRUE(channels.dispatch(ticker_frame));

    // 3. Dispatch a trade frame
    const auto trade_frame = nlohmann::json{
        { "time", 2 }, { "channel", "spot.trades" }, { "event", "update" }, { "result", { { "id", "12345" } } }
    };
    EXPECT_TRUE(channels.dispatch(trade_frame));

    // 4. Verify each callback received the correct data
    EXPECT_EQ("BTC_USDT", last_ticker);
    EXPECT_EQ("12345", last_trade);
}

// ---------------------------------------------------------------------------
// Test 6: BuildSubscribeMsgFormat — verify JSON structure
// ---------------------------------------------------------------------------
TEST(GateWsChannelsTest, BuildSubscribeMsgFormat)
{
    const GateWsChannels channels;
    const std::vector<std::string> payload = { "BTC_USDT", "ETH_USDT" };

    const auto msg = channels.build_subscribe_msg("spot.tickers", payload);

    // 1. Verify required fields exist
    EXPECT_TRUE(msg.contains("time"));
    EXPECT_TRUE(msg.contains("channel"));
    EXPECT_TRUE(msg.contains("event"));
    EXPECT_TRUE(msg.contains("payload"));

    // 2. Verify field values
    EXPECT_EQ("spot.tickers", msg["channel"].get<std::string>());
    EXPECT_EQ("subscribe", msg["event"].get<std::string>());
    EXPECT_EQ(2u, msg["payload"].size());
    EXPECT_EQ("BTC_USDT", msg["payload"][0].get<std::string>());
    EXPECT_EQ("ETH_USDT", msg["payload"][1].get<std::string>());

    // 3. Verify timestamp is a plausible Unix seconds value (> 2020-01-01)
    EXPECT_GT(msg["time"].get<std::int64_t>(), 1577836800);
}

// ---------------------------------------------------------------------------
// Test 7: BuildUnsubscribeMsgFormat — verify JSON structure
// ---------------------------------------------------------------------------
TEST(GateWsChannelsTest, BuildUnsubscribeMsgFormat)
{
    const GateWsChannels channels;
    const std::vector<std::string> payload = { "BTC_USDT" };

    const auto msg = channels.build_unsubscribe_msg("spot.tickers", payload);

    EXPECT_EQ("spot.tickers", msg["channel"].get<std::string>());
    EXPECT_EQ("unsubscribe", msg["event"].get<std::string>());
    EXPECT_EQ(1u, msg["payload"].size());
    EXPECT_EQ("BTC_USDT", msg["payload"][0].get<std::string>());
}

// ---------------------------------------------------------------------------
// Test 8: BuildPongFromSpotPing — verify pong response for spot ping
// ---------------------------------------------------------------------------
TEST(GateWsChannelsTest, BuildPongFromSpotPing)
{
    // 1. Simulate a server spot ping frame
    const auto ping = nlohmann::json{ { "time", 1700000000 }, { "channel", "spot.ping" } };

    // 2. Build the pong reply (default MarketType::Spot)
    const auto pong = GateWsChannels::build_pong(ping);

    // 3. Verify pong has the same timestamp and spot.pong channel
    EXPECT_EQ(1700000000, pong["time"].get<std::int64_t>());
    EXPECT_EQ("spot.pong", pong["channel"].get<std::string>());
}

// ---------------------------------------------------------------------------
// Test 8b: BuildPongFromFuturesPing — verify pong for futures ping
// ---------------------------------------------------------------------------
TEST(GateWsChannelsTest, BuildPongFromFuturesPing)
{
    // 1. Simulate a server futures ping frame
    const auto ping = nlohmann::json{ { "time", 1700000500 }, { "channel", "futures.ping" } };

    // 2. Build the pong reply with Futures market type
    const auto pong = GateWsChannels::build_pong(ping, MarketType::Futures);

    // 3. Verify pong has futures.pong channel (dynamically derived from futures.ping)
    EXPECT_EQ(1700000500, pong["time"].get<std::int64_t>());
    EXPECT_EQ("futures.pong", pong["channel"].get<std::string>());
}

// ---------------------------------------------------------------------------
// Test 8c: BuildPong_PreservesTime — time field is always copied from ping
// ---------------------------------------------------------------------------
TEST(GateWsChannelsTest, BuildPong_PreservesTime)
{
    // Various time values should be preserved exactly
    for (const auto ts : { 0, 1, 1577836800, 1700000000, 2147483647 })
    {
        const auto ping = nlohmann::json{ { "time", ts }, { "channel", "spot.ping" } };
        const auto pong = GateWsChannels::build_pong(ping);
        EXPECT_EQ(ts, pong["time"].get<std::int64_t>()) << "time mismatch for ts=" << ts;
    }
}

// ---------------------------------------------------------------------------
// Test 8d: BuildPong_DynamicChannel — channel is derived from input, not hardcoded
// ---------------------------------------------------------------------------
TEST(GateWsChannelsTest, BuildPong_DynamicChannel)
{
    // Even if we pass Spot as market type, a futures.ping channel should produce futures.pong
    // because the channel is derived from the input ping frame's channel field.
    const auto ping = nlohmann::json{ { "time", 42 }, { "channel", "futures.ping" } };
    const auto pong = GateWsChannels::build_pong(ping, MarketType::Spot);
    EXPECT_EQ("futures.pong", pong["channel"].get<std::string>());

    // Conversely, spot.ping with Futures market type should still produce spot.pong
    const auto ping2 = nlohmann::json{ { "time", 43 }, { "channel", "spot.ping" } };
    const auto pong2 = GateWsChannels::build_pong(ping2, MarketType::Futures);
    EXPECT_EQ("spot.pong", pong2["channel"].get<std::string>());
}

// ---------------------------------------------------------------------------
// Test 9: ActiveChannels — verify list of subscribed channel names
// ---------------------------------------------------------------------------
TEST(GateWsChannelsTest, ActiveChannels)
{
    GateWsChannels channels;

    // 1. Initially empty
    EXPECT_TRUE(channels.active_channels().empty());

    // 2. Subscribe to two channels
    channels.subscribe("spot.tickers", { "BTC_USDT" }, [](const nlohmann::json &, const nlohmann::json &) {});
    channels.subscribe("spot.trades", { "BTC_USDT" }, [](const nlohmann::json &, const nlohmann::json &) {});

    // 3. Verify both appear in active list
    const auto active = channels.active_channels();
    EXPECT_EQ(2u, active.size());

    // 4. Both channel names should be present (order not guaranteed with unordered_map)
    const bool has_tickers =
        std::any_of(active.begin(), active.end(), [](const std::string &s) { return "spot.tickers" == s; });
    const bool has_trades =
        std::any_of(active.begin(), active.end(), [](const std::string &s) { return "spot.trades" == s; });
    EXPECT_TRUE(has_tickers);
    EXPECT_TRUE(has_trades);
}

// ---------------------------------------------------------------------------
// Test 10: GetPayload — verify payload retrieval for re-subscription
// ---------------------------------------------------------------------------
TEST(GateWsChannelsTest, GetPayload)
{
    GateWsChannels channels;

    // 1. Unknown channel returns empty
    EXPECT_TRUE(channels.get_payload("spot.tickers").empty());

    // 2. Subscribe with specific payload
    const std::vector<std::string> payload = { "BTC_USDT", "ETH_USDT" };
    channels.subscribe("spot.tickers", payload, [](const nlohmann::json &, const nlohmann::json &) {});

    // 3. Verify payload is returned correctly
    const auto retrieved = channels.get_payload("spot.tickers");
    EXPECT_EQ(2u, retrieved.size());
    EXPECT_EQ("BTC_USDT", retrieved[0]);
    EXPECT_EQ("ETH_USDT", retrieved[1]);
}

// ---------------------------------------------------------------------------
// Test 11: DispatchWithNullResult — frame with no "result" field
// ---------------------------------------------------------------------------
TEST(GateWsChannelsTest, DispatchWithNullResult)
{
    GateWsChannels channels;
    bool callback_invoked = false;
    nlohmann::json received_result;

    channels.subscribe("spot.tickers",
        { "BTC_USDT" },
        [&](const nlohmann::json &result, const nlohmann::json &)
        {
            received_result = result;
            callback_invoked = true;
        });

    // Dispatch a frame without a "result" field
    const auto frame = nlohmann::json{ { "time", 1 }, { "channel", "spot.tickers" }, { "event", "subscribe" } };
    EXPECT_TRUE(channels.dispatch(frame));
    EXPECT_TRUE(callback_invoked);

    // result should be a null JSON value
    EXPECT_TRUE(received_result.is_null());
}

// ---------------------------------------------------------------------------
// Test 12: SubscribeReplacesExisting — re-subscribe overwrites previous entry
// ---------------------------------------------------------------------------
TEST(GateWsChannelsTest, SubscribeReplacesExisting)
{
    GateWsChannels channels;
    std::atomic<int> first_count{ 0 };
    std::atomic<int> second_count{ 0 };

    // 1. Subscribe with first callback
    channels.subscribe("spot.tickers",
        { "BTC_USDT" },
        [&](const nlohmann::json &, const nlohmann::json &) { first_count.fetch_add(1); });

    // 2. Re-subscribe with different callback and payload
    channels.subscribe("spot.tickers",
        { "ETH_USDT" },
        [&](const nlohmann::json &, const nlohmann::json &) { second_count.fetch_add(1); });

    // 3. Dispatch — only the second callback should fire
    const auto frame =
        nlohmann::json{ { "time", 1 }, { "channel", "spot.tickers" }, { "event", "update" }, { "result", nullptr } };
    EXPECT_TRUE(channels.dispatch(frame));

    EXPECT_EQ(0, first_count.load());
    EXPECT_EQ(1, second_count.load());

    // 4. Payload should be the new one
    const auto payload = channels.get_payload("spot.tickers");
    EXPECT_EQ(1u, payload.size());
    EXPECT_EQ("ETH_USDT", payload[0]);
}

} // namespace pulse::exchange::test
