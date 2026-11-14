#pragma once
// JsonRpcServer.hpp — TCP control socket with newline-delimited JSON-RPC 2.0
//
// Binding:
//   - Localhost-only by default (ControlConfig.bindAddress)
//   - One accept thread + one handler thread per connection
//   - Framing: one JSON object per line (read_until '\n', strips \r)
//
// Protocol (JSON-RPC 2.0):
//   - Request      → {jsonrpc:"2.0", id, method, params}
//   - Notification → same without id (no response)
//   - Response     → {jsonrpc:"2.0", id, result|error}
//   - Errors: -32700 parse, -32600 invalid request, -32601 method not found,
//             -32603 internal (data carries the PulseError code + message)
//
// Server push (connection-scoped, NOT part of the method registry):
//   - subscribe_market / unsubscribe_market → channel "kline"  (market.kline)
//   - subscribe_orders  / unsubscribe_orders → channel "order"  (order.update)
//   Each connection keeps ONE subscription per channel, so a client can watch
//   closed bars and order lifecycle events over the same socket. These methods
//   are deliberately socket-only: MCP is request/response and has no
//   server-push channel.
//
// dispatchLine() is a pure static function — unit-tested without sockets.

#include "core/PulseError.hpp"

#include <asio/ip/tcp.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pulse::control
{

class EngineServices;

using RpcResult = Result<nlohmann::json>;
using MethodHandler = std::function<RpcResult(const nlohmann::json &params)>;
using MethodRegistry = std::unordered_map<std::string, MethodHandler>;

class JsonRpcServer
{
  public:
    /// Transport security for the control socket.
    ///
    /// Defaults reproduce the historical behaviour: plaintext on loopback with
    /// no authentication. TLS and the token are independent switches.
    struct Security
    {
        bool tlsEnabled = false;
        std::string certPath;
        std::string keyPath;
        std::string caPath;
        bool requireClientCert = false;
        std::string token; ///< Empty disables authentication.
    };

    JsonRpcServer(std::string bind_address, std::uint16_t port,
                  MethodRegistry registry);

    ~JsonRpcServer();

    // Non-copyable, non-movable (owns threads).
    JsonRpcServer(const JsonRpcServer &) = delete;
    JsonRpcServer &operator=(const JsonRpcServer &) = delete;
    JsonRpcServer(JsonRpcServer &&) = delete;
    JsonRpcServer &operator=(JsonRpcServer &&) = delete;

    /// Configure transport security. Must be called before start(); the
    /// default Security keeps the socket plaintext and unauthenticated.
    void setSecurity(Security security);

    /// Bind the acceptor. Returns false on bind failure (port in use etc.) or
    /// when TLS was requested but the context cannot be built.
    bool start();

    /// Close the acceptor + all sessions, join threads.
    void stop();

    [[nodiscard]] std::uint16_t port() const { return m_port; }

    /// A published server-push event, matched against per-session filters.
    struct Topic
    {
        std::string channel;     ///< "kline" | "order".
        std::string symbol;      ///< e.g. "BTC_USDT".
        std::string market_type; ///< "spot" | "futures" | "cfd".
    };

    /// Push a JSON-RPC notification to every session subscribed to `topic`.
    ///
    /// A session is a target when ANY of its channel subscriptions matches the
    /// topic's channel, market type and symbol set; a session subscribed to
    /// several channels still receives each event exactly once.
    ///
    /// Thread-safety: callable from any thread — the feed I/O thread calls it
    /// once per closed bar and the order path calls it per order event. It
    /// ONLY enqueues into bounded per-subscriber queues, so a slow or dead
    /// client can never block the market-data path. When a subscriber's queue
    /// is full the oldest notification is dropped and counted in that
    /// session's `dropped` counter.
    void publish(const Topic &topic, const std::string &method,
                 const nlohmann::json &params);

    /// Pure dispatch of one JSON-RPC line → response line ("" for
    /// notifications). Exposed for unit tests.
    [[nodiscard]] static std::string dispatchLine(const std::string &line,
                                                  const MethodRegistry &registry);

  private:
    /// Holds asio::ssl::context so the public header never pulls OpenSSL into
    /// translation units that only use the server's interface.
    struct TlsState;

    /// Per-session push filter. An empty `market_type` or empty `symbols` set
    /// is a wildcard. Sessions hold these in a map keyed by channel, so one
    /// connection can subscribe to several channels independently.
    struct Subscription
    {
        std::string market_type;
        std::unordered_set<std::string> symbols;
    };

    /// Per-connection state shared by the reader loop and the notification
    /// writer. A session is registered with the server only while it has at
    /// least one active subscription, so an idle client costs nothing on
    /// publish().
    struct Session
    {
        std::mutex mutex; ///< Guards queue / stopped / subscriptions.
        std::condition_variable cv;
        std::deque<std::string> queue; ///< Bounded outgoing notifications.
        bool stopped{ false };
        std::uint64_t dropped{ 0 };

        /// Active push filters, keyed by channel ("kline" | "order"). The key
        /// IS the channel, so a subscribe replaces only that channel's filter
        /// and unsubscribing one channel leaves the others intact.
        std::unordered_map<std::string, Subscription> subscriptions;

        std::mutex writeMutex; ///< Serialises ALL writes to the socket.
    };

    /// Handle the four connection-scoped push methods for one connection:
    /// subscribe_market / unsubscribe_market (channel "kline") and
    /// subscribe_orders / unsubscribe_orders (channel "order").
    /// Returns true when the line was a subscription request (and `response`
    /// holds the reply); false lets the caller fall through to dispatchLine.
    bool handleSubscriptionRequest(const std::string &line,
                                   const std::shared_ptr<Session> &session,
                                   std::string &response);

    void registerSession(const std::shared_ptr<Session> &session);
    void unregisterSession(const Session *session);

    void acceptLoop();
    void handleSessionPlain(std::shared_ptr<asio::ip::tcp::socket> sock);
    void handleSessionTls(std::shared_ptr<void> stream);

    /// Read the first line and verify the bearer token when one is configured.
    /// Returns false after writing the 9106 failure, in which case the caller
    /// must close the connection.
    template <typename Stream> bool authenticate(Stream &stream);

    /// The normal request/response loop, shared by the plaintext and TLS paths
    /// because asio::ssl::stream satisfies the same sync-stream concept.
    template <typename Stream> void runLoop(Stream &stream);

    /// Build the SSL context, or return false after logging.
    bool initTls();

    /// SHA-256 of the configured token, computed once in start().
    void initTokenDigest();

    std::string m_bindAddress;
    std::uint16_t m_port;
    MethodRegistry m_registry;
    Security m_security;

    asio::io_context m_ioCtx;
    asio::ip::tcp::acceptor m_acceptor;
    std::thread m_acceptThread;
    std::atomic<bool> m_running{ false };

    std::unique_ptr<TlsState> m_tls;

    /// SHA-256(expected token). Empty token → m_tokenRequired stays false.
    std::array<unsigned char, 32> m_tokenDigest{};
    bool m_tokenRequired = false;

    /// Sessions with an active subscription (guarded by m_subscriberMutex).
    std::mutex m_subscriberMutex;
    std::vector<std::shared_ptr<Session>> m_subscribers;

    std::mutex m_sessionMutex;
    std::vector<std::thread> m_sessions;
    /// One closer per live session; stop() invokes them to unblock a blocking
    /// read (SSL or plain) from another thread.
    std::vector<std::function<void()>> m_sessionClosers;
};

/// Build the full 18-method registry from an EngineServices instance.
[[nodiscard]] MethodRegistry makeMethodRegistry(EngineServices &services);

} // namespace pulse::control
