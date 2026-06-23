#pragma once
// proxy_tunnel.hpp — HTTP CONNECT tunneling for WebSocket through HTTP proxy
//
// websocketpp does not natively support HTTP proxies. ProxyTunnel creates a
// local TCP listener that accepts connections from websocketpp, establishes
// an HTTP CONNECT tunnel through the proxy to the real WSS server, and
// relays data bidirectionally using blocking I/O in separate threads.
//
// Usage:
//   1. Construct with proxy URL, target host, and target port
//   2. Call start() — binds to a random local port, starts accept thread
//   3. Get local_ws_url() — use this as the websocketpp connect URL
//   4. Call stop() or destroy — closes acceptor, joins all threads
//
// Thread safety:
//   - start() and stop() are not thread-safe (call from same thread)
//   - Internal relay threads are tracked and joined in stop()

#include "core/config.hpp"

#include <asio/ip/tcp.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pulse::exchange
{

// ---------------------------------------------------------------------------
// WsUrlParts — parsed components of a WSS URL
// ---------------------------------------------------------------------------
struct WsUrlParts
{
    std::string host;
    std::uint16_t port;
    std::string path;
};

/// Parse a WSS URL into host, port, and path components.
/// Default port is 443, default path is "/".
///   "wss://api.gateio.ws/ws/v4/" → {"api.gateio.ws", 443, "/ws/v4/"}
[[nodiscard]] WsUrlParts parseWsUrl(const std::string &url);

/// Detect proxy URL from config or environment variables.
/// Priority: config.proxyUrl → HTTPS_PROXY → HTTP_PROXY → empty string.
[[nodiscard]] std::string detectProxyUrl(const ExchangeConfig &config);

// ---------------------------------------------------------------------------
// ProxyTunnel — HTTP CONNECT tunneling for WebSocket through HTTP proxy
// ---------------------------------------------------------------------------
class ProxyTunnel
{
  public:
    ProxyTunnel(const std::string &proxy_url,
                const std::string &target_host,
                std::uint16_t target_port,
                const std::string &target_path = "/");

    ~ProxyTunnel();

    ProxyTunnel(const ProxyTunnel &) = delete;
    ProxyTunnel &operator=(const ProxyTunnel &) = delete;

    /// Start the local listener and accept thread. Returns local WS URL.
    [[nodiscard]] std::string start();

    /// Stop the tunnel — close acceptor, sockets, and join all threads.
    void stop();

    /// Returns the local port assigned by the OS.
    [[nodiscard]] std::uint16_t localPort() const;

  private:
    /// Handle a single proxied connection: establish tunnel, then relay data.
    void handleConnection(asio::ip::tcp::socket local_sock,
                           const std::string &proxy_host,
                           const std::string &proxy_port);

    /// Relay data from source to sink until EOF or error (plain socket).
    static void relayData(asio::ip::tcp::socket &source,
                           asio::ip::tcp::socket &sink);

    std::string m_proxyUrl;
    std::string m_targetHost;
    std::uint16_t m_targetPort;
    std::string m_targetPath;
    asio::io_context m_ioCtx;
    std::unique_ptr<asio::ip::tcp::acceptor> m_acceptor;
    std::thread m_acceptThread;
    std::thread m_connectionThread; ///< Tracked handleConnection thread (Bug #1 fix).
    std::atomic<bool> m_running{ false };
    std::uint16_t m_localPort{ 0 };

    std::mutex m_relayMutex;
    std::vector<std::thread> m_relayThreads;
    std::vector<std::shared_ptr<asio::ip::tcp::socket>> m_relaySockets;

    /// Socket used during handleConnection's asio::connect() phase.
    /// Tracked so stop() can close it to unblock a pending connection.
    std::shared_ptr<asio::ip::tcp::socket> m_connectingSock;
};

} // namespace pulse::exchange
