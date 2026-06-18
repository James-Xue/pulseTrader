#pragma once
// gate_ws_client.hpp — Gate.io v4 WebSocket client (Layer 1 Exchange)
//
// Manages a persistent WebSocket connection to Gate.io's v4 WS endpoint.
// Features:
//   1. Auto-reconnect with exponential backoff + jitter
//   2. Server ping/pong handling (spot.ping → spot.pong)
//   3. Dynamic channel subscribe/unsubscribe via GateWsChannels registry
//   4. Private channel authentication via HMAC-SHA512 (reuses gate_auth)
//   5. Dedicated I/O thread with cooperative cancellation (std::jthread + stop_token)
//
// Thread safety:
//   - subscribe/unsubscribe can be called from any thread (posts to I/O thread)
//   - state() is lock-free (atomic load)
//   - channels() returns a reference to the thread-safe GateWsChannels registry

#include "core/config.hpp"
#include "exchange/gate_ws_channels.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace pulse::exchange
{

// Forward declaration — defined in gate_ws_client.cpp
struct WsInternal;

// ---------------------------------------------------------------------------
// WsConnectionState — WebSocket connection lifecycle states
// ---------------------------------------------------------------------------
enum class WsConnectionState : std::uint8_t
{
    Disconnected, ///< Not connected, not attempting to connect.
    Connecting,   ///< TCP/TLS handshake in progress.
    Connected,    ///< WebSocket open, ready to send/receive.
};

// ---------------------------------------------------------------------------
// detail — Pure functions exposed for unit testing (no WebSocket connection)
// ---------------------------------------------------------------------------
namespace detail
{

/// Compute reconnection delay with exponential backoff and ±25% jitter.
///
/// Formula: min(base_ms * 2^attempt, max_ms) * random(0.75, 1.25)
///
/// Parameters:
///   1. attempt — zero-based reconnect attempt number
///   2. base_ms — base delay in milliseconds (e.g. 1000)
///   3. max_ms  — maximum delay cap in milliseconds (e.g. 30000)
[[nodiscard]] std::uint32_t compute_backoff_ms(std::uint32_t attempt, std::uint32_t base_ms, std::uint32_t max_ms);

/// Build the Gate.io WS private-channel authentication JSON block.
///
/// Format: {"method": "api_key", "KEY": "<key>", "SIGN": "<hmac_hex>", "time": <unix>}
/// The signature is HMAC-SHA512 of "channel=<ch>&event=<ev>&time=<ts>" using the API secret.
///
/// Parameters:
///   1. api_key    — Gate.io API key
///   2. api_secret — Gate.io API secret (HMAC key)
///   3. channel    — channel being subscribed to (e.g. "spot.orders")
///   4. event      — event type (typically "subscribe")
[[nodiscard]] nlohmann::json build_ws_auth(const std::string &api_key,
    const std::string &api_secret,
    const std::string &channel,
    const std::string &event);

} // namespace detail

// ---------------------------------------------------------------------------
// GateWsClient — WebSocket client for Gate.io v4 real-time feeds
//
// Usage:
//   GateWsClient client(config);
//   client.subscribe("spot.tickers", {"BTC_USDT"}, [](auto &result, auto &) { ... });
//   client.start();    // spawns I/O thread, connects, subscribes
//   ...
//   client.stop();     // closes connection, joins I/O thread
// ---------------------------------------------------------------------------
class GateWsClient
{
  public:
    /// Construct a WebSocket client from exchange configuration.
    ///
    /// Uses config.wsUrl for the endpoint, config.wsReconnectBaseMs / wsReconnectMaxMs
    /// for backoff parameters, and config.apiKey / apiSecret for private channel auth.
    ///
    /// Does NOT start the I/O thread — call start() explicitly.
    explicit GateWsClient(const ExchangeConfig &config);

    /// Destructor calls stop() if the I/O thread is still running.
    ~GateWsClient();

    GateWsClient(const GateWsClient &) = delete;
    GateWsClient &operator=(const GateWsClient &) = delete;

    /// Start the WebSocket I/O thread and initiate connection.
    ///
    /// The I/O thread runs asio::io_context and handles:
    ///   1. WebSocket connect/reconnect with exponential backoff
    ///   2. Incoming message parsing and channel dispatch
    ///   3. Server ping/pong handling
    ///
    /// Safe to call multiple times — subsequent calls are no-ops if already started.
    void start();

    /// Initiate graceful shutdown.
    ///
    /// 1. Signals the I/O thread to stop via std::stop_token
    /// 2. Closes the WebSocket connection
    /// 3. Joins the I/O thread (blocks until it exits)
    ///
    /// Safe to call multiple times.
    void stop();

    /// Subscribe to a public channel with a callback.
    ///
    /// The subscription is registered immediately. If the client is connected,
    /// a subscribe message is sent to the server. If not yet connected, the
    /// subscription will be sent on the next successful connection.
    void subscribe(const std::string &channel, const std::vector<std::string> &payload, ChannelCallback callback);

    /// Subscribe to a private (authenticated) channel.
    ///
    /// Adds HMAC-SHA512 authentication to the subscribe message.
    /// Requires valid apiKey and apiSecret in the ExchangeConfig.
    void
    subscribe_private(const std::string &channel, const std::vector<std::string> &payload, ChannelCallback callback);

    /// Unsubscribe from a channel.
    ///
    /// Sends an unsubscribe message to the server if connected,
    /// and removes the callback from the registry.
    void unsubscribe(const std::string &channel);

    /// Returns the current connection state (lock-free atomic load).
    [[nodiscard]] WsConnectionState state() const;

    /// Access the channel subscription registry.
    ///
    /// Useful for inspecting active channels or for testing.
    /// The registry itself is thread-safe (shared_mutex).
    [[nodiscard]] GateWsChannels &channels();

  private:
    ExchangeConfig config_;
    GateWsChannels channels_;
    std::atomic<WsConnectionState> state_{ WsConnectionState::Disconnected };
    std::jthread io_thread_;
    std::shared_ptr<WsInternal> internal_;

    /// I/O thread body — runs the WebSocket event loop.
    ///
    /// 1. Initialise websocketpp client
    /// 2. Register open/close/message/error handlers
    /// 3. Connect to the WS endpoint
    /// 4. Run the asio event loop
    /// 5. On disconnect: backoff and reconnect (unless stop requested)
    void run_io_loop(std::stop_token stop_token);
};

} // namespace pulse::exchange
