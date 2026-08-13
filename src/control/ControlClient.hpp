#pragma once
// ControlClient.hpp — Blocking JSON-RPC client for the engine control socket
//
// Used by the `cli` (remote attach) and `mcp` (stdio bridge) subcommands.
// One blocking call() per request; connect timeout via deadline timer.

#include "core/PulseError.hpp"

#include <asio/ip/tcp.hpp>
#include <asio/steady_timer.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace pulse::control
{

class ControlClient
{
  public:
    ControlClient() = default;
    ~ControlClient();

    // Non-copyable (owns a socket).
    ControlClient(const ControlClient &) = delete;
    ControlClient &operator=(const ControlClient &) = delete;
    ControlClient(ControlClient &&) = delete;
    ControlClient &operator=(ControlClient &&) = delete;

    /// Connect to the engine control socket. Returns false on failure.
    bool connect(const std::string &host, std::uint16_t port,
                 int timeout_ms = 3000);

    void disconnect();

    [[nodiscard]] bool connected() const;

    /// Send one JSON-RPC request, wait for the response, return result.
    /// The response "result" is returned on success; JSON-RPC errors and
    /// transport failures return PulseError.
    [[nodiscard]] Result<nlohmann::json>
    call(const std::string &method,
         const nlohmann::json &params = nlohmann::json::object());

  private:
    std::unique_ptr<asio::io_context> m_ioCtx;
    std::unique_ptr<asio::ip::tcp::socket> m_sock;
    std::string m_lineBuffer;
    std::uint64_t m_nextId{ 1 };
};

} // namespace pulse::control
