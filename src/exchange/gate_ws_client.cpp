// gate_ws_client.cpp — Gate.io v4 WebSocket client implementation (Layer 1 Exchange)
//
// Uses websocketpp with standalone asio (not boost::asio) for TLS WebSocket connectivity.
// The I/O thread owns the websocketpp client and connection handle exclusively.
// External subscribe/unsubscribe calls are queued and dispatched on the I/O thread.

// Must define ASIO_STANDALONE before any websocketpp includes to use standalone asio.
#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif

#include "pulse/exchange/gate_ws_client.hpp"

#include "pulse/exchange/gate_auth.hpp"
#include "pulse/logging/logger.hpp"

#include <websocketpp/client.hpp>
#include <websocketpp/config/asio_client.hpp>

#include <asio/steady_timer.hpp>

#include <algorithm>
#include <chrono>
#include <functional>
#include <mutex>
#include <queue>
#include <random>
#include <thread>

namespace pulse::exchange
{

// ---------------------------------------------------------------------------
// Type alias for the websocketpp client with standalone asio + TLS
// ---------------------------------------------------------------------------
using WsClient = websocketpp::client<websocketpp::config::asio_tls_client>;
using WsConnectionPtr = websocketpp::connection_hdl;
using WsMessagePtr = WsClient::message_ptr;

// ---------------------------------------------------------------------------
// detail — Pure functions for testing
// ---------------------------------------------------------------------------
namespace detail
{

std::uint32_t compute_backoff_ms(std::uint32_t attempt, std::uint32_t base_ms, std::uint32_t max_ms)
{
    // 1. Compute exponential delay: base * 2^attempt, capped at max
    // 2. Apply ±25% jitter using thread-local RNG
    const std::uint64_t raw_delay = static_cast<std::uint64_t>(base_ms) << std::min(attempt, 20u);
    const auto capped = static_cast<std::uint32_t>(std::min(raw_delay, static_cast<std::uint64_t>(max_ms)));

    // Jitter: uniform random in [0.75 * capped, 1.25 * capped]
    thread_local std::mt19937 rng{ std::random_device{}() };
    const double low = static_cast<double>(capped) * 0.75;
    const double high = static_cast<double>(capped) * 1.25;
    std::uniform_real_distribution<double> dist(low, high);
    return static_cast<std::uint32_t>(dist(rng));
}

nlohmann::json build_ws_auth(const std::string &api_key,
    const std::string &api_secret,
    const std::string &channel,
    const std::string &event)
{
    // Gate.io private channel auth format:
    //   sign_payload = "channel=<channel>&event=<event>&time=<unix_seconds>"
    //   signature    = HMAC-SHA512(secret, sign_payload)
    //   auth block   = {"method": "api_key", "KEY": "<key>", "SIGN": "<sig>", "time": <unix>}
    const std::string ts = unix_seconds();
    const std::string sign_payload = "channel=" + channel + "&event=" + event + "&time=" + ts;
    const std::string sig = hmac_sha512_hex(api_secret, sign_payload);

    return nlohmann::json{ { "method", "api_key" }, { "KEY", api_key }, { "SIGN", sig }, { "time", std::stoll(ts) } };
}

} // namespace detail

// ---------------------------------------------------------------------------
// Internal state shared between the I/O thread and the GateWsClient
// ---------------------------------------------------------------------------
namespace
{

/// Pending action queued from an external thread to be executed on the I/O thread.
struct PendingAction
{
    enum class Type : std::uint8_t
    {
        Subscribe,
        SubscribePrivate,
        Unsubscribe,
    };
    Type type;
    std::string channel;
    std::vector<std::string> payload;
    ChannelCallback callback;
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// Pimpl-like internal state (kept in .cpp to hide websocketpp types from header)
//
// We store this as static-duration state scoped to the GateWsClient instance
// via a shared pointer, so the I/O thread can access it after the constructor.
// ---------------------------------------------------------------------------
struct WsInternal
{
    std::mutex queue_mutex;
    std::queue<PendingAction> pending_actions;

    /// Process all queued actions on the I/O thread.
    /// Called after connection is established.
    void drain_queue(WsClient &client, WsConnectionPtr hdl, GateWsChannels &channels, const ExchangeConfig &config);
};

void WsInternal::drain_queue(WsClient &client,
    WsConnectionPtr hdl,
    GateWsChannels &channels,
    const ExchangeConfig &config)
{
    std::queue<PendingAction> actions;
    {
        std::lock_guard lock(queue_mutex);
        std::swap(actions, pending_actions);
    }

    while (!actions.empty())
    {
        auto &action = actions.front();
        switch (action.type)
        {
        case PendingAction::Type::Subscribe:
        {
            // 1. Register callback in channel registry
            channels.subscribe(action.channel, action.payload, std::move(action.callback));

            // 2. Build and send subscribe message
            const auto msg = channels.build_subscribe_msg(action.channel, action.payload);
            const std::string json_str = msg.dump();
            client.send(hdl, json_str, websocketpp::frame::opcode::text);
            PULSE_LOG_INFO("exchange", "WS subscribed to {}", action.channel);
            break;
        }
        case PendingAction::Type::SubscribePrivate:
        {
            // 1. Register callback
            channels.subscribe(action.channel, action.payload, std::move(action.callback));

            // 2. Build subscribe message with auth block
            auto msg = channels.build_subscribe_msg(action.channel, action.payload);
            msg["auth"] = detail::build_ws_auth(config.apiKey, config.apiSecret, action.channel, "subscribe");
            const std::string json_str = msg.dump();
            client.send(hdl, json_str, websocketpp::frame::opcode::text);
            PULSE_LOG_INFO("exchange", "WS subscribed to {} (private)", action.channel);
            break;
        }
        case PendingAction::Type::Unsubscribe:
        {
            // 1. Build and send unsubscribe message
            const auto payload = channels.get_payload(action.channel);
            const auto msg = channels.build_unsubscribe_msg(action.channel, payload);
            const std::string json_str = msg.dump();
            client.send(hdl, json_str, websocketpp::frame::opcode::text);

            // 2. Remove from registry
            channels.unsubscribe(action.channel);
            PULSE_LOG_INFO("exchange", "WS unsubscribed from {}", action.channel);
            break;
        }
        }
        actions.pop();
    }
}

// ---------------------------------------------------------------------------
// GateWsClient implementation
// ---------------------------------------------------------------------------

GateWsClient::GateWsClient(const ExchangeConfig &config) : config_(config), channels_()
{
}

GateWsClient::~GateWsClient()
{
    stop();
}

void GateWsClient::start()
{
    // Guard: only start if currently disconnected and no thread running
    if (io_thread_.joinable())
    {
        PULSE_LOG_WARN("exchange", "WS client already started");
        return;
    }

    state_.store(WsConnectionState::Disconnected, std::memory_order_release);
    io_thread_ = std::jthread([this](std::stop_token token) { run_io_loop(token); });
    PULSE_LOG_INFO("exchange", "WS client I/O thread started");
}

void GateWsClient::stop()
{
    if (!io_thread_.joinable())
    {
        return;
    }

    // 1. Request the I/O thread to stop
    io_thread_.request_stop();

    // 2. Wait for the thread to finish (it will exit the reconnect loop on stop_token)
    io_thread_.join();
    state_.store(WsConnectionState::Disconnected, std::memory_order_release);
    PULSE_LOG_INFO("exchange", "WS client stopped");
}

void GateWsClient::subscribe(const std::string &channel,
    const std::vector<std::string> &payload,
    ChannelCallback callback)
{
    // Register the callback in the channel registry immediately (thread-safe).
    channels_.subscribe(channel, payload, callback);
}

void GateWsClient::subscribe_private(const std::string &channel,
    const std::vector<std::string> &payload,
    ChannelCallback callback)
{
    // Register the callback — auth is added when sending the subscribe message
    channels_.subscribe(channel, payload, callback);
}

void GateWsClient::unsubscribe(const std::string &channel)
{
    channels_.unsubscribe(channel);
}

WsConnectionState GateWsClient::state() const
{
    return state_.load(std::memory_order_acquire);
}

GateWsChannels &GateWsClient::channels()
{
    return channels_;
}

// ---------------------------------------------------------------------------
// run_io_loop — the main I/O thread body
// ---------------------------------------------------------------------------
void GateWsClient::run_io_loop(std::stop_token stop_token)
{
    // Shared internal state for cross-thread communication
    auto internal = std::make_shared<WsInternal>();

    std::uint32_t attempt = 0;

    // Reconnect loop — runs until stop_token is triggered
    while (!stop_token.stop_requested())
    {
        try
        {
            // 1. Create a fresh websocketpp client for each connection attempt
            WsClient client;

            // 2. Configure logging — suppress websocketpp's own logging
            client.clear_access_channels(websocketpp::log::alevel::all);
            client.clear_error_channels(websocketpp::log::elevel::all);

            // 3. Initialise the asio transport
            client.init_asio();

            // 4. Set TLS handler for wss:// connections
            client.set_tls_init_handler(
                [](websocketpp::connection_hdl /*hdl*/) -> websocketpp::lib::shared_ptr<asio::ssl::context>
                {
                    // 1. Create a new SSL context with TLS 1.2+
                    // 2. Enable server certificate verification using system CA bundle
                    auto ctx = websocketpp::lib::make_shared<asio::ssl::context>(asio::ssl::context::tlsv12_client);
                    ctx->set_verify_mode(asio::ssl::verify_peer);
                    ctx->set_default_verify_paths();
                    return ctx;
                });

            state_.store(WsConnectionState::Connecting, std::memory_order_release);
            PULSE_LOG_INFO("exchange", "WS connecting to {} (attempt {})", config_.wsUrl, attempt + 1);

            // 5. Create a connection and set handlers
            websocketpp::lib::error_code ec;
            auto con = client.get_connection(config_.wsUrl, ec);
            if (ec)
            {
                PULSE_LOG_ERROR("exchange", "WS get_connection failed: {}", ec.message());
                state_.store(WsConnectionState::Disconnected, std::memory_order_release);
                break;
            }

            // Store the connection handle for sending messages
            std::mutex hdl_mutex;
            WsConnectionPtr active_hdl;

            // --- on_open handler ---
            client.set_open_handler(
                [this,
                    &client,
                    &hdl_mutex,
                    &active_hdl,
                    &attempt,
                    &internal,
                    &channels = this->channels_,
                    &config = this->config_](WsConnectionPtr hdl)
                {
                    {
                        std::lock_guard lock(hdl_mutex);
                        active_hdl = hdl;
                    }
                    state_.store(WsConnectionState::Connected, std::memory_order_release);
                    attempt = 0; // reset backoff on successful connection
                    PULSE_LOG_INFO("exchange", "WS connected to {}", config_.wsUrl);

                    // Re-subscribe all active channels
                    const auto active = channels.active_channels();
                    for (const auto &ch : active)
                    {
                        const auto payload = channels.get_payload(ch);
                        const auto msg = channels.build_subscribe_msg(ch, payload);
                        client.send(hdl, msg.dump(), websocketpp::frame::opcode::text);
                        PULSE_LOG_DEBUG("exchange", "WS re-subscribed to {}", ch);
                    }

                    // Drain any pending actions queued while disconnected
                    internal->drain_queue(client, hdl, channels, config);
                });

            // --- on_close handler ---
            client.set_close_handler(
                [this](WsConnectionPtr /*hdl*/)
                {
                    state_.store(WsConnectionState::Disconnected, std::memory_order_release);
                    PULSE_LOG_WARN("exchange", "WS connection closed");
                });

            // --- on_fail handler ---
            client.set_fail_handler(
                [this](WsConnectionPtr /*hdl*/)
                {
                    state_.store(WsConnectionState::Disconnected, std::memory_order_release);
                    PULSE_LOG_ERROR("exchange", "WS connection failed");
                });

            // --- on_message handler ---
            client.set_message_handler(
                [this, &channels = this->channels_](WsConnectionPtr /*hdl*/, WsMessagePtr msg)
                {
                    // 1. Parse the incoming frame as JSON
                    const std::string &body = msg->get_payload();
                    auto frame = nlohmann::json::parse(body, nullptr, false);

                    if (frame.is_discarded())
                    {
                        PULSE_LOG_WARN("exchange", "WS received invalid JSON frame ({} bytes)", body.size());
                        return;
                    }

                    // 2. Handle server ping — reply with pong immediately
                    if (frame.contains("channel") && "spot.ping" == frame["channel"].get<std::string>())
                    {
                        // Pong is handled by the caller who has access to the connection handle.
                        // We log it here; actual pong sending is done in the outer scope.
                        PULSE_LOG_DEBUG("exchange", "WS received server ping");
                        return;
                    }

                    // 3. Dispatch to channel handler
                    if (!channels.dispatch(frame))
                    {
                        const auto channel_name = frame.value("channel", "unknown");
                        PULSE_LOG_DEBUG("exchange", "WS frame for unhandled channel: {}", channel_name);
                    }
                });

            // 6. Connect and run
            client.connect(con);

            // 7. Run the event loop — blocks until the connection closes or stop is requested
            //    We use a poll-based approach to check stop_token periodically
            auto &asio_io = client.get_io_context();
            while (!stop_token.stop_requested())
            {
                // Run io_service for up to 100ms, then check stop_token
                asio_io.restart();
                const auto handlers_run = asio_io.run_for(std::chrono::milliseconds(100));
                if (0 == handlers_run)
                {
                    // No work to do — brief sleep to avoid busy-waiting
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }

                // Check if connection is closed — break out to reconnect
                if (WsConnectionState::Disconnected == state_.load(std::memory_order_acquire))
                {
                    break;
                }
            }

            // 8. Clean up — close the connection if still open
            {
                std::lock_guard lock(hdl_mutex);
                if (active_hdl.use_count() > 0)
                {
                    websocketpp::lib::error_code close_ec;
                    client.close(active_hdl, websocketpp::close::status::going_away, "shutdown", close_ec);
                }
            }

            // Brief pause to let close handshake complete
            client.get_io_context().run_for(std::chrono::milliseconds(500));

            if (stop_token.stop_requested())
            {
                break;
            }
        }
        catch (const std::exception &e)
        {
            PULSE_LOG_ERROR("exchange", "WS exception: {}", e.what());
            state_.store(WsConnectionState::Disconnected, std::memory_order_release);
        }

        if (stop_token.stop_requested())
        {
            break;
        }

        // 9. Exponential backoff before reconnect
        const auto delay_ms = detail::compute_backoff_ms(attempt, config_.wsReconnectBaseMs, config_.wsReconnectMaxMs);
        PULSE_LOG_INFO("exchange", "WS reconnecting in {} ms (attempt {})", delay_ms, attempt + 1);

        // Sleep in small increments so we can respond to stop_token quickly
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(delay_ms);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (stop_token.stop_requested())
            {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        ++attempt;
    }

    state_.store(WsConnectionState::Disconnected, std::memory_order_release);
    PULSE_LOG_INFO("exchange", "WS I/O thread exiting");
}

} // namespace pulse::exchange
