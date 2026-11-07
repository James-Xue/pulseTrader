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
// dispatchLine() is a pure static function — unit-tested without sockets.

#include "core/PulseError.hpp"

#include <asio/ip/tcp.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
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

    /// Pure dispatch of one JSON-RPC line → response line ("" for
    /// notifications). Exposed for unit tests.
    [[nodiscard]] static std::string dispatchLine(const std::string &line,
                                                  const MethodRegistry &registry);

  private:
    /// Holds asio::ssl::context so the public header never pulls OpenSSL into
    /// translation units that only use the server's interface.
    struct TlsState;

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

    std::mutex m_sessionMutex;
    std::vector<std::thread> m_sessions;
    /// One closer per live session; stop() invokes them to unblock a blocking
    /// read (SSL or plain) from another thread.
    std::vector<std::function<void()>> m_sessionClosers;
};

/// Build the full 18-method registry from an EngineServices instance.
[[nodiscard]] MethodRegistry makeMethodRegistry(EngineServices &services);

} // namespace pulse::control
