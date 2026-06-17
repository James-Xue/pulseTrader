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
#include <asio/ip/tcp.hpp>
#include <asio/read_until.hpp>
#include <asio/write.hpp>

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
// Proxy tunnel — HTTP CONNECT tunneling for WebSocket through HTTP proxy
//
// websocketpp does not natively support HTTP proxies. This class creates a
// local TCP listener that accepts connections from websocketpp, establishes
// an HTTP CONNECT tunnel through the proxy to the real WSS server, and
// relays data bidirectionally using blocking I/O in separate threads.
//
// Usage:
//   1. Construct with proxy URL, target host, and target port
//   2. Call start() — binds to a random local port, starts accept thread
//   3. Get local_ws_url() — use this as the websocketpp connect URL
//   4. Destroy — closes acceptor, joins threads
// ---------------------------------------------------------------------------
class ProxyTunnel
{
public:
    ProxyTunnel(const std::string &proxy_url, const std::string &target_host, std::uint16_t target_port)
        : proxy_url_(proxy_url), target_host_(target_host), target_port_(target_port)
    {
    }

    ~ProxyTunnel()
    {
        stop();
    }

    /// Start the local listener and accept thread. Returns local WS URL.
    std::string start()
    {
        // 1. Parse proxy URL → host:port
        std::string proxy_host;
        std::string proxy_port = "8080";
        auto scheme_end = proxy_url_.find("://");
        std::string host_part = (std::string::npos != scheme_end) ? proxy_url_.substr(scheme_end + 3) : proxy_url_;
        auto colon_pos = host_part.find(':');
        if (std::string::npos != colon_pos)
        {
            proxy_host = host_part.substr(0, colon_pos);
            proxy_port = host_part.substr(colon_pos + 1);
        }
        else
        {
            proxy_host = host_part;
        }

        // 2. Bind local acceptor to a random available port
        local_port_ = 0;
        acceptor_ = std::make_unique<asio::ip::tcp::acceptor>(
            io_ctx_, asio::ip::tcp::endpoint(asio::ip::address::from_string("127.0.0.1"), 0));
        acceptor_->listen();
        local_port_ = acceptor_->local_endpoint().port();

        // 3. Start accept thread (blocking I/O, independent of io_ctx_)
        running_ = true;
        accept_thread_ = std::thread([this, proxy_host, proxy_port]()
        {
            while (running_)
            {
                try
                {
                    asio::ip::tcp::socket local_sock(io_ctx_);
                    acceptor_->accept(local_sock);
                    std::thread(&ProxyTunnel::handle_connection, this, std::move(local_sock), proxy_host, proxy_port)
                        .detach();
                }
                catch (const std::exception &)
                {
                    if (!running_)
                    {
                        break;
                    }
                }
            }
        });

        return "wss://127.0.0.1:" + std::to_string(local_port_) + "/";
    }

    /// Stop the tunnel — close acceptor and join accept thread
    void stop()
    {
        running_ = false;
        if (acceptor_)
        {
            asio::error_code ec;
            acceptor_->close(ec);
        }
        if (accept_thread_.joinable())
        {
            accept_thread_.join();
        }
    }

    /// Returns the local port assigned by the OS
    std::uint16_t local_port() const
    {
        return local_port_;
    }

private:
    /// Handle a single proxied connection: establish tunnel, then relay data
    void handle_connection(asio::ip::tcp::socket local_sock,
        const std::string &proxy_host,
        const std::string &proxy_port)
    {
        try
        {
            // 1. Connect to the HTTP proxy
            asio::ip::tcp::socket remote_sock(io_ctx_);
            asio::ip::tcp::resolver resolver(io_ctx_);
            auto endpoints = resolver.resolve(proxy_host, proxy_port);
            asio::connect(remote_sock, endpoints);

            // 2. Send HTTP CONNECT request to establish tunnel
            const std::string target = target_host_ + ":" + std::to_string(target_port_);
            const std::string connect_req = "CONNECT " + target + " HTTP/1.1\r\n"
                "Host: " + target + "\r\n"
                "Proxy-Connection: keep-alive\r\n\r\n";
            asio::write(remote_sock, asio::buffer(connect_req));

            // 3. Read proxy response — expect "HTTP/1.x 200 Connection established"
            asio::streambuf response_buf;
            asio::read_until(remote_sock, response_buf, "\r\n\r\n");
            std::istream response_stream(&response_buf);
            std::string status_line;
            std::getline(response_stream, status_line);

            if (std::string::npos == status_line.find("200"))
            {
                PULSE_LOG_ERROR("exchange", "WS proxy CONNECT failed: {}", status_line);
                return;
            }

            PULSE_LOG_DEBUG("exchange", "WS proxy tunnel established to {}", target);

            // 4. Bidirectional relay using blocking I/O in two threads
            auto local_ptr = std::make_shared<asio::ip::tcp::socket>(std::move(local_sock));
            auto remote_ptr = std::make_shared<asio::ip::tcp::socket>(std::move(remote_sock));

            // Local → Remote (websocketpp → proxy → server)
            std::thread([local_ptr, remote_ptr]()
            {
                relay_data(*local_ptr, *remote_ptr);
            }).detach();

            // Remote → Local (server → proxy → websocketpp)
            std::thread([local_ptr, remote_ptr]()
            {
                relay_data(*remote_ptr, *local_ptr);
                // When remote→local ends, close both to unblock the other direction
                asio::error_code ec;
                local_ptr->shutdown(asio::ip::tcp::socket::shutdown_both, ec);
                remote_ptr->shutdown(asio::ip::tcp::socket::shutdown_both, ec);
            }).detach();
        }
        catch (const std::exception &e)
        {
            PULSE_LOG_ERROR("exchange", "WS proxy tunnel error: {}", e.what());
        }
    }

    /// Relay data from source to sink until EOF or error
    static void relay_data(asio::ip::tcp::socket &source, asio::ip::tcp::socket &sink)
    {
        try
        {
            char buf[8192];
            while (true)
            {
                asio::error_code ec;
                const std::size_t bytes = source.read_some(asio::buffer(buf), ec);
                if (ec)
                {
                    break;
                }
                asio::write(sink, asio::buffer(buf, bytes), ec);
                if (ec)
                {
                    break;
                }
            }
        }
        catch (...)
        {
        }
    }

    std::string proxy_url_;
    std::string target_host_;
    std::uint16_t target_port_;
    asio::io_context io_ctx_;
    std::unique_ptr<asio::ip::tcp::acceptor> acceptor_;
    std::thread accept_thread_;
    std::atomic<bool> running_{ false };
    std::uint16_t local_port_ = 0;
};

// ---------------------------------------------------------------------------
// Helper: detect proxy URL from config or environment variables
// ---------------------------------------------------------------------------
std::string detect_proxy_url(const ExchangeConfig &config)
{
    if (!config.proxyUrl.empty())
    {
        return config.proxyUrl;
    }
    if (const char *proxy = std::getenv("HTTPS_PROXY"); proxy)
    {
        return proxy;
    }
    if (const char *proxy = std::getenv("HTTP_PROXY"); proxy)
    {
        return proxy;
    }
    return {};
}

// ---------------------------------------------------------------------------
// Helper: extract host and port from a WSS URL
//   "wss://api.gateio.ws/ws/v4/" → {"api.gateio.ws", 443}
// ---------------------------------------------------------------------------
std::pair<std::string, std::uint16_t> parse_ws_url(const std::string &url)
{
    std::string host;
    std::uint16_t port = 443;

    auto scheme_end = url.find("://");
    std::string rest = (std::string::npos != scheme_end) ? url.substr(scheme_end + 3) : url;
    auto path_start = rest.find('/');
    std::string authority = (std::string::npos != path_start) ? rest.substr(0, path_start) : rest;

    auto colon_pos = authority.find(':');
    if (std::string::npos != colon_pos)
    {
        host = authority.substr(0, colon_pos);
        port = static_cast<std::uint16_t>(std::stoi(authority.substr(colon_pos + 1)));
    }
    else
    {
        host = authority;
    }

    return { host, port };
}

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

    // --- Proxy tunnel setup ---
    // If a proxy is configured (config or env var), create a local TCP tunnel.
    // websocketpp connects to the local tunnel, which forwards through the proxy
    // via HTTP CONNECT to the real WSS server.
    const std::string proxy_url = detect_proxy_url(config_);
    std::string connect_url = config_.wsUrl;
    std::string real_host;
    std::unique_ptr<ProxyTunnel> tunnel;

    if (!proxy_url.empty())
    {
        auto [host, port] = parse_ws_url(config_.wsUrl);
        real_host = host;
        tunnel = std::make_unique<ProxyTunnel>(proxy_url, host, port);
        connect_url = tunnel->start();
        PULSE_LOG_INFO("exchange", "WS proxy tunnel active: {} → {} (via {})",
            connect_url, config_.wsUrl, proxy_url);
    }

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

            state_.store(WsConnectionState::Connecting, std::memory_order_release);
            PULSE_LOG_INFO("exchange", "WS connecting to {} (attempt {})", connect_url, attempt + 1);

            // 5. Create a connection and set handlers
            websocketpp::lib::error_code ec;
            auto con = client.get_connection(connect_url, ec);
            if (ec)
            {
                PULSE_LOG_ERROR("exchange", "WS get_connection failed: {}", ec.message());
                state_.store(WsConnectionState::Disconnected, std::memory_order_release);
                break;
            }

            // SNI hostname is set in the TLS handler above via SSL_CTX_set_tlsext_host_name

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
