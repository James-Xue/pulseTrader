// gate_ws_client.cpp — Gate.io v4 WebSocket client implementation (Layer 1 Exchange)
//
// Uses websocketpp with standalone asio (not boost::asio) for TLS WebSocket connectivity.
// The I/O thread owns the websocketpp client and connection handle exclusively.
// External subscribe/unsubscribe calls are queued and dispatched on the I/O thread.

// Must define ASIO_STANDALONE before any websocketpp includes to use standalone asio.
#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif

#include "exchange/gate_ws_client.hpp"

#include "exchange/endpoint_router.hpp"
#include "exchange/gate_auth.hpp"
#include "exchange/proxy_tunnel.hpp"
#include "logging/logger.hpp"

#include <websocketpp/client.hpp>
#include <websocketpp/config/asio_client.hpp>

#include <asio/steady_timer.hpp>
#include <asio/ssl.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>

#include <functional>
#include <mutex>
#include <queue>
#include <random>
#include <sstream>
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

    // Active connection handle (set in on_open, cleared on close/fail)
    std::mutex hdl_mutex;
    WsConnectionPtr active_hdl;

    // Pointer to the active WsClient (valid while client.run() is executing).
    // Used by send_queued() to send messages from external threads.
    WsClient *client_ptr{ nullptr };

    // Pointer to the active io_context (valid while client.run() is executing).
    // Used by stop() to post a close operation and unblock the event loop.
    asio::io_context *io_ctx_ptr{ nullptr };

    /// Process all queued actions on the I/O thread.
    /// Called after connection is established.
    void drain_queue(WsClient &client, WsConnectionPtr hdl, GateWsChannels &channels, const ExchangeConfig &config);

    /// Send all queued actions using the active connection handle.
    /// Called from external threads (subscribe/unsubscribe) when already connected.
    void send_queued(GateWsChannels &channels, const ExchangeConfig &config);
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
            // Callback already registered by subscribe(); just send the WS message.
            const auto msg = channels.build_subscribe_msg(action.channel, action.payload);
            const std::string json_str = msg.dump();
            client.send(hdl, json_str, websocketpp::frame::opcode::text);
            PULSE_LOG_INFO("exchange", "WS subscribed to {}", action.channel);
            break;
        }
        case PendingAction::Type::SubscribePrivate:
        {
            // Callback already registered by subscribe_private(); just send with auth.
            auto msg = channels.build_subscribe_msg(action.channel, action.payload);
            msg["auth"] = detail::build_ws_auth(config.apiKey, config.apiSecret, action.channel, "subscribe");
            const std::string json_str = msg.dump();
            client.send(hdl, json_str, websocketpp::frame::opcode::text);
            PULSE_LOG_INFO("exchange", "WS subscribed to {} (private)", action.channel);
            break;
        }
        case PendingAction::Type::Unsubscribe:
        {
            // Callback already removed by unsubscribe(); just send the WS message.
            const auto payload = channels.get_payload(action.channel);
            const auto msg = channels.build_unsubscribe_msg(action.channel, payload);
            client.send(hdl, msg.dump(), websocketpp::frame::opcode::text);
            PULSE_LOG_INFO("exchange", "WS unsubscribed from {}", action.channel);
            break;
        }
        }
        actions.pop();
    }
}

void WsInternal::send_queued(GateWsChannels &channels, const ExchangeConfig &config)
{
    std::queue<PendingAction> actions;
    {
        std::lock_guard lock(queue_mutex);
        std::swap(actions, pending_actions);
    }

    if (actions.empty())
    {
        return;
    }

    // Capture the handle and client pointer under lock.
    WsConnectionPtr hdl;
    WsClient *cli = nullptr;
    {
        std::lock_guard hlock(hdl_mutex);
        hdl = active_hdl;
        cli = client_ptr;
    }

    if (hdl.expired() || nullptr == cli)
    {
        // Connection lost between state check and handle capture — re-queue everything.
        std::lock_guard lock(queue_mutex);
        while (!actions.empty())
        {
            pending_actions.push(std::move(actions.front()));
            actions.pop();
        }
        return;
    }

    while (!actions.empty())
    {
        auto &action = actions.front();
        switch (action.type)
        {
        case PendingAction::Type::Subscribe:
        {
            // Callback already registered by subscribe(); just send the WS message.
            const auto msg = channels.build_subscribe_msg(action.channel, action.payload);
            cli->send(hdl, msg.dump(), websocketpp::frame::opcode::text);
            PULSE_LOG_INFO("exchange", "WS subscribed to {}", action.channel);
            break;
        }
        case PendingAction::Type::SubscribePrivate:
        {
            // Callback already registered by subscribe_private(); just send with auth.
            auto msg = channels.build_subscribe_msg(action.channel, action.payload);
            msg["auth"] = detail::build_ws_auth(config.apiKey, config.apiSecret, action.channel, "subscribe");
            cli->send(hdl, msg.dump(), websocketpp::frame::opcode::text);
            PULSE_LOG_INFO("exchange", "WS subscribed to {} (private)", action.channel);
            break;
        }
        case PendingAction::Type::Unsubscribe:
        {
            // Callback already removed by unsubscribe(); just send the WS message.
            const auto payload = channels.get_payload(action.channel);
            const auto msg = channels.build_unsubscribe_msg(action.channel, payload);
            cli->send(hdl, msg.dump(), websocketpp::frame::opcode::text);
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

GateWsClient::GateWsClient(const ExchangeConfig &config, MarketType market_type)
    : config_(config), market_type_(market_type), channels_(), internal_(std::make_shared<WsInternal>())
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

    // 2. Force-stop the io_context to unblock client.run()
    {
        std::lock_guard lock(internal_->hdl_mutex);
        if (internal_->io_ctx_ptr)
        {
            internal_->io_ctx_ptr->stop();
        }
    }

    // 3. Wait for the thread to finish
    io_thread_.join();
    state_.store(WsConnectionState::Disconnected, std::memory_order_release);
    PULSE_LOG_INFO("exchange", "WS client stopped");
}

void GateWsClient::subscribe(const std::string &channel,
    const std::vector<std::string> &payload,
    ChannelCallback callback)
{
    // 1. Register the callback in the channel registry immediately (thread-safe).
    channels_.subscribe(channel, payload, std::move(callback));

    // 2. Queue the subscribe message for sending on the I/O thread.
    {
        std::lock_guard lock(internal_->queue_mutex);
        internal_->pending_actions.push(
            PendingAction{ PendingAction::Type::Subscribe, channel, payload, nullptr });
    }

    // 3. If already connected, send immediately.
    if (WsConnectionState::Connected == state_.load(std::memory_order_acquire))
    {
        internal_->send_queued(channels_, config_);
    }
}

void GateWsClient::subscribe_private(const std::string &channel,
    const std::vector<std::string> &payload,
    ChannelCallback callback)
{
    channels_.subscribe(channel, payload, std::move(callback));

    {
        std::lock_guard lock(internal_->queue_mutex);
        internal_->pending_actions.push(
            PendingAction{ PendingAction::Type::SubscribePrivate, channel, payload, nullptr });
    }

    if (WsConnectionState::Connected == state_.load(std::memory_order_acquire))
    {
        internal_->send_queued(channels_, config_);
    }
}

void GateWsClient::unsubscribe(const std::string &channel)
{
    // Remove from channel registry immediately.
    channels_.unsubscribe(channel);

    // Queue the unsubscribe message for sending on the I/O thread.
    {
        std::lock_guard lock(internal_->queue_mutex);
        internal_->pending_actions.push(
            PendingAction{ PendingAction::Type::Unsubscribe, channel, {}, nullptr });
    }

    if (WsConnectionState::Connected == state_.load(std::memory_order_acquire))
    {
        internal_->send_queued(channels_, config_);
    }
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
    std::uint32_t attempt = 0;

    // --- Proxy tunnel setup ---
    // If a proxy is configured (config or env var), create a local TCP tunnel.
    // websocketpp connects to the local tunnel, which forwards through the proxy
    // via HTTP CONNECT to the real WSS server.
    const std::string proxy_url = detect_proxy_url(config_);
    const std::string ws_url = EndpointRouter::select_ws_url(config_, market_type_);
    std::string connect_url = ws_url;
    std::string real_host;
    std::unique_ptr<ProxyTunnel> tunnel;

    if (!proxy_url.empty())
    {
        auto parts = parse_ws_url(ws_url);
        real_host = parts.host;
        tunnel = std::make_unique<ProxyTunnel>(proxy_url, parts.host, parts.port, parts.path);
        connect_url = tunnel->start();
        PULSE_LOG_INFO("exchange", "WS proxy tunnel active: {} → {} (via {})",
            connect_url, ws_url, proxy_url);
    }

    // Reconnect loop — runs until stop_token is triggered
    while (!stop_token.stop_requested())
    {
        try
        {
            // 1. Create a fresh websocketpp client for each connection attempt
            WsClient client;

            // 2. Configure logging — enable minimal logging to debug handshake
            client.clear_access_channels(websocketpp::log::alevel::all);
            client.set_access_channels(websocketpp::log::alevel::connect | websocketpp::log::alevel::disconnect);
            client.clear_error_channels(websocketpp::log::elevel::all);
            client.set_error_channels(websocketpp::log::elevel::all);

            // 3. Initialise the asio transport
            client.init_asio();
            internal_->io_ctx_ptr = &client.get_io_context();

            // 4. Set TLS handler for wss:// connections
            //    When using a proxy tunnel, we connect via wss:// to 127.0.0.1
            //    so hostname verification is disabled (cert is for the real server).
            //    The proxy-to-server leg is still TLS-encrypted.
            const bool use_proxy = !real_host.empty();
            client.set_tls_init_handler(
                [use_proxy](websocketpp::connection_hdl /*hdl*/) -> websocketpp::lib::shared_ptr<asio::ssl::context>
                {
                    auto ctx = websocketpp::lib::make_shared<asio::ssl::context>(asio::ssl::context::tlsv12_client);
                    if (use_proxy)
                    {
                        // Proxy tunnel: skip certificate verification
                        // (cert hostname won't match 127.0.0.1)
                        ctx->set_verify_mode(asio::ssl::verify_none);
                    }
                    else
                    {
                        // Direct connection: full verification with CA bundle
                        ctx->set_verify_mode(asio::ssl::verify_peer);
                        ctx->set_default_verify_paths();
                    }
                    return ctx;
                });

            // --- Set all handlers BEFORE get_connection() ---
            // --- on_open handler ---
            client.set_open_handler(
                [this,
                    &client,
                    &attempt,
                    &internal = this->internal_,
                    &channels = this->channels_,
                    &config = this->config_](WsConnectionPtr hdl)
                {
                    PULSE_LOG_DEBUG("exchange", "on_open handler called");
                    {
                        std::lock_guard lock(internal->hdl_mutex);
                        internal->active_hdl = hdl;
                    }
                    state_.store(WsConnectionState::Connected, std::memory_order_release);
                    attempt = 0; // reset backoff on successful connection
                    PULSE_LOG_INFO("exchange", "WS connected (market={})",
                        MarketType::Futures == market_type_ ? "futures" : "spot");

                    // Re-subscribe all active channels
                    const auto active = channels.active_channels();
                    PULSE_LOG_DEBUG("exchange", "Active channels: {}", active.size());
                    for (const auto &ch : active)
                    {
                        const auto payload = channels.get_payload(ch);
                        const auto msg = channels.build_subscribe_msg(ch, payload);
                        client.send(hdl, msg.dump(), websocketpp::frame::opcode::text);
                        PULSE_LOG_INFO("exchange", "WS re-subscribed to {}", ch);
                    }

                    // Drain any pending actions queued while disconnected
                    internal->drain_queue(client, hdl, channels, config);
                });

            // --- on_close handler ---
            client.set_close_handler(
                [this, &internal = this->internal_](WsConnectionPtr /*hdl*/)
                {
                    {
                        std::lock_guard lock(internal->hdl_mutex);
                        internal->active_hdl.reset();
                    }
                    state_.store(WsConnectionState::Disconnected, std::memory_order_release);
                    PULSE_LOG_WARN("exchange", "WS connection closed");
                });

            // --- on_fail handler ---
            client.set_fail_handler(
                [this, &internal = this->internal_](WsConnectionPtr /*hdl*/)
                {
                    {
                        std::lock_guard lock(internal->hdl_mutex);
                        internal->active_hdl.reset();
                    }
                    state_.store(WsConnectionState::Disconnected, std::memory_order_release);
                    PULSE_LOG_ERROR("exchange", "WS connection failed");
                });

            // --- on_message handler ---
            client.set_message_handler(
                [this, &client, &internal = this->internal_, &channels = this->channels_](
                    WsConnectionPtr hdl, WsMessagePtr msg)
                {
                    // 1. Parse the incoming frame as JSON
                    const std::string &body = msg->get_payload();

                    // Diagnostic: log every incoming frame at INFO level.
                    // This is essential for detecting whether the server is sending data at all.
                    // Truncate to 200 chars to avoid flooding logs with large orderbook snapshots.
                    const auto channel_preview = body.size() <= 200 ? body : body.substr(0, 200) + "...";
                    PULSE_LOG_INFO("exchange", "WS RX {} bytes: {}", body.size(), channel_preview);

                    auto frame = nlohmann::json::parse(body, nullptr, false);

                    if (frame.is_discarded())
                    {
                        PULSE_LOG_WARN("exchange", "WS received invalid JSON frame ({} bytes)", body.size());
                        return;
                    }

                    // 2. Handle server ping — reply with pong immediately
                    //    Supports both spot (JSON spot.ping) and futures (JSON futures.ping or RFC 6455)
                    const std::string ping_ch = EndpointRouter::ping_channel(market_type_);
                    if (frame.contains("channel") && ping_ch == frame["channel"].get<std::string>())
                    {
                        PULSE_LOG_DEBUG("exchange", "WS received server ping ({})", ping_ch);
                        const auto pong = GateWsChannels::build_pong(frame, market_type_);
                        client.send(hdl, pong.dump(), websocketpp::frame::opcode::text);
                        PULSE_LOG_DEBUG("exchange", "WS sent pong ({})",
                            EndpointRouter::pong_channel(market_type_));
                        return;
                    }

                    // 3. Dispatch to channel handler
                    if (!channels.dispatch(frame))
                    {
                        const auto channel_name = frame.value("channel", "unknown");
                        PULSE_LOG_WARN("exchange", "WS frame for unhandled channel: {}", channel_name);
                    }
                });

            state_.store(WsConnectionState::Connecting, std::memory_order_release);
            PULSE_LOG_INFO("exchange", "WS connecting to {} (attempt {})", connect_url, attempt + 1);

            // 5. Create a connection
            websocketpp::lib::error_code ec;
            auto con = client.get_connection(connect_url, ec);
            if (ec)
            {
                PULSE_LOG_ERROR("exchange", "WS get_connection failed: {}", ec.message());
                state_.store(WsConnectionState::Disconnected, std::memory_order_release);
                break;
            }

            // When using proxy tunnel, override Host header to the real server
            // (websocketpp would otherwise send 127.0.0.1:<port> from connect_url)
            if (use_proxy)
            {
                con->replace_header("Host", real_host);
                PULSE_LOG_DEBUG("exchange", "Set Host header to: {}", real_host);
            }

            // 6. Connect and run
            client.connect(con);

            // 7. Store client pointer for cross-thread send, then run the event loop
            internal_->client_ptr = &client;
            PULSE_LOG_DEBUG("exchange", "Entering client.run()");
            client.run();
            PULSE_LOG_DEBUG("exchange", "client.run() returned");
            internal_->client_ptr = nullptr;
            internal_->io_ctx_ptr = nullptr;

            // Check if we should stop or reconnect
            if (stop_token.stop_requested())
            {
                break;
            }

            // Connection closed - loop to reconnect
            const auto backoff = detail::compute_backoff_ms(attempt++, 1000, 30000);
            PULSE_LOG_INFO("exchange", "WS reconnecting in {} ms", backoff);
            std::this_thread::sleep_for(std::chrono::milliseconds(backoff));

            // 8. Clean up — close the connection if still open
            {
                std::lock_guard lock(internal_->hdl_mutex);
                if (internal_->active_hdl.use_count() > 0)
                {
                    websocketpp::lib::error_code close_ec;
                    client.close(internal_->active_hdl, websocketpp::close::status::going_away, "shutdown", close_ec);
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
    if (tunnel)
    {
        tunnel->stop();
        tunnel.reset();
    }
}

} // namespace pulse::exchange
