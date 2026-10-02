#pragma once
// gate_ws_channels.hpp — Gate.io WebSocket channel subscription registry (Layer 1 Exchange)
//
// Manages typed channel subscriptions for the Gate.io v4 WebSocket API.
// Each channel has a name (e.g. "spot.tickers"), a payload (e.g. ["BTC_USDT"]),
// and a callback that fires when a matching frame arrives.
//
// Key properties:
//   1. Thread-safe: uses std::shared_mutex for concurrent reads, exclusive writes
//   2. No websocketpp dependency: pure logic for subscription management
//   3. Builds Gate.io subscribe/unsubscribe JSON messages
//   4. Builds pong replies for server ping frames
//   5. Tracks active channels for re-subscription on reconnect
//
// Thread safety:
//   - Multiple readers (dispatch, activeChannels) can proceed concurrently
//   - Writers (subscribe, unsubscribe) hold exclusive lock
//   - Callbacks are invoked under a shared read lock — they must NOT call back
//     into subscribe/unsubscribe (would deadlock)

#include "core/types.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace pulse::exchange
{

// ---------------------------------------------------------------------------
// ChannelCallback — invoked when a matching frame arrives
//
// Parameters:
//   1. result     — the "result" field of the incoming frame (may be null)
//   2. full_frame — the complete JSON frame for advanced inspection
// ---------------------------------------------------------------------------
using ChannelCallback = std::function<void(const nlohmann::json &result, const nlohmann::json &full_frame)>;

// ---------------------------------------------------------------------------
// GateWsChannels — subscription registry and message builder
//
// Responsibilities:
//   1. Store channel → (payloads, callback) mappings
//   2. Dispatch incoming frames to the matching callback
//   3. Build subscribe/unsubscribe JSON messages per Gate.io v4 protocol
//   4. Build pong replies for server-initiated pings
//   5. Expose active subscriptions for re-subscription after reconnect
// ---------------------------------------------------------------------------
class GateWsChannels
{
  public:
    /// Register a callback for a Gate.io channel.
    ///
    /// Payloads ACCUMULATE per channel: each distinct payload is remembered so
    /// that a reconnect can re-subscribe every one of them. This matters for
    /// channels whose payload is positional and cannot be batched across
    /// symbols (e.g. futures.candlesticks ["1m", "<contract>"], which is
    /// subscribed once per symbol). Subscribing an identical (channel, payload)
    /// pair again is a no-op for the payload list.
    ///
    /// The callback is REPLACED on each call (last registration wins) — all
    /// subscriptions of one channel receive the same frames, so a single
    /// callback per channel is the dispatch model.
    ///
    /// Parameters:
    ///   1. channel  — Gate.io channel name (e.g. "spot.tickers")
    ///   2. payload  — subscription parameters (e.g. ["BTC_USDT", "ETH_USDT"])
    ///   3. callback — function invoked when a matching update frame arrives
    ///
    /// Thread safety: exclusive write lock.
    void subscribe(const std::string &channel, const std::vector<std::string> &payload, ChannelCallback callback);

    /// Remove a channel subscription — all accumulated payloads.
    ///
    /// After this call, dispatch() will no longer invoke the callback for this channel.
    /// No-op if the channel is not currently subscribed.
    ///
    /// Thread safety: exclusive write lock.
    void unsubscribe(const std::string &channel);

    /// Dispatch an incoming frame to the matching channel callback.
    ///
    /// The frame must be a JSON object with a "channel" field.
    /// If a matching subscription exists, the callback is invoked with
    /// frame["result"] and the full frame.
    ///
    /// Returns true if a handler was found and invoked, false otherwise.
    ///
    /// Thread safety: shared read lock. Callback must NOT call subscribe/unsubscribe.
    [[nodiscard]] bool dispatch(const nlohmann::json &frame);

    /// Build a Gate.io v4 subscribe JSON message.
    ///
    /// Format: {"time": <unix>, "channel": "<name>", "event": "subscribe", "payload": [...]}
    [[nodiscard]] nlohmann::json buildSubscribeMsg(const std::string &channel,
        const std::vector<std::string> &payload) const;

    /// Build a Gate.io v4 unsubscribe JSON message.
    ///
    /// Format: {"time": <unix>, "channel": "<name>", "event": "unsubscribe", "payload": [...]}
    [[nodiscard]] nlohmann::json buildUnsubscribeMsg(const std::string &channel,
        const std::vector<std::string> &payload) const;

    /// Build a pong reply for a server-initiated ping.
    ///
    /// Gate.io spot sends    {"time": <int>, "channel": "spot.ping"}    → reply {"time": <int>, "channel": "spot.pong"}
    /// Gate.io futures sends {"time": <int>, "channel": "futures.ping"} → reply {"time": <int>, "channel": "futures.pong"}
    ///
    /// The pong channel is derived dynamically from the ping frame's channel
    /// by replacing ".ping" with ".pong", so the MarketType parameter is used
    /// only as a fallback when the channel field is absent.
    [[nodiscard]] static nlohmann::json buildPong(const nlohmann::json &ping_frame,
        MarketType mt = MarketType::Spot);

    /// Returns a list of currently subscribed channel names (unique).
    ///
    /// Thread safety: shared read lock.
    [[nodiscard]] std::vector<std::string> activeChannels() const;

    /// Returns every payload registered for a channel, in subscription order.
    ///
    /// One entry per distinct payload (e.g. a per-symbol candlesticks channel
    /// yields one payload per symbol). Used to build per-payload unsubscribe
    /// messages. Returns an empty vector if the channel is not subscribed.
    ///
    /// Thread safety: shared read lock.
    [[nodiscard]] std::vector<std::vector<std::string>> payloadsFor(const std::string &channel) const;

    /// Build one subscribe message per registered (channel, payload) pair —
    /// the complete re-subscription set for a fresh connection.
    ///
    /// Payloads must be replayed as separate messages because positional
    /// payloads cannot be merged across symbols. Order: channel order is
    /// unspecified (hash map), payload order within a channel is
    /// subscription order.
    ///
    /// Thread safety: shared read lock.
    [[nodiscard]] std::vector<nlohmann::json> buildResubscribeMsgs() const;

  private:
    /// Internal storage for one channel subscription.
    struct ChannelEntry
    {
        std::vector<std::vector<std::string>> payloads; ///< All distinct payloads, subscription order.
        ChannelCallback callback;                       ///< Last registration wins.
    };

    mutable std::shared_mutex m_mutex;
    std::unordered_map<std::string, ChannelEntry> m_channels;
};

} // namespace pulse::exchange
