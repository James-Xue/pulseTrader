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
    JsonRpcServer(std::string bind_address, std::uint16_t port,
                  MethodRegistry registry);

    ~JsonRpcServer();

    // Non-copyable, non-movable (owns threads).
    JsonRpcServer(const JsonRpcServer &) = delete;
    JsonRpcServer &operator=(const JsonRpcServer &) = delete;
    JsonRpcServer(JsonRpcServer &&) = delete;
    JsonRpcServer &operator=(JsonRpcServer &&) = delete;

    /// Bind the acceptor. Returns false on bind failure (port in use etc.).
    bool start();

    /// Close the acceptor + all sessions, join threads.
    void stop();

    [[nodiscard]] std::uint16_t port() const { return m_port; }

    /// Pure dispatch of one JSON-RPC line → response line ("" for
    /// notifications). Exposed for unit tests.
    [[nodiscard]] static std::string dispatchLine(const std::string &line,
                                                  const MethodRegistry &registry);

  private:
    void acceptLoop();
    void handleSession(std::shared_ptr<asio::ip::tcp::socket> sock);

    std::string m_bindAddress;
    std::uint16_t m_port;
    MethodRegistry m_registry;

    asio::io_context m_ioCtx;
    asio::ip::tcp::acceptor m_acceptor;
    std::thread m_acceptThread;
    std::atomic<bool> m_running{ false };

    std::mutex m_sessionMutex;
    std::vector<std::thread> m_sessions;
    std::vector<std::shared_ptr<asio::ip::tcp::socket>> m_sessionSockets;
};

/// Build the full 16-method registry from an EngineServices instance.
[[nodiscard]] MethodRegistry makeMethodRegistry(EngineServices &services);

} // namespace pulse::control
