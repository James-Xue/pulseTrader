#pragma once
// McpServer.hpp — stdio MCP (Model Context Protocol) server
//
// Speaks the MCP 2025 stdio transport: newline-delimited JSON-RPC 2.0 on
// stdin/stdout. Tools are the 16 control-plane methods; each tools/call
// is forwarded to the engine's control socket via the Backend.
//
// Stdio hygiene (critical): the host process must route spdlog console
// output AWAY from stdout (mcp mode forces toConsole=false) and must never
// write to std::cout — the protocol stream is the only thing on stdout.

#include "control/JsonRpcServer.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <iostream>
#include <string>

namespace pulse::control
{

class McpServer
{
  public:
    /// Backend: forward (method, params) to the engine control socket.
    /// Unit tests inject a fake.
    using Backend = std::function<RpcResult(const std::string &method,
                                            const nlohmann::json &params)>;

    explicit McpServer(Backend backend)
        : m_backend{ std::move(backend) }
    {
    }

    /// Run the stdio loop until EOF or a notifications/exit.
    void run(std::istream &in, std::ostream &out);

    /// Handle one incoming line → response line ("" for notifications).
    /// Testable without stdin/stdout.
    [[nodiscard]] static std::string handleLine(const std::string &line,
                                                Backend &backend,
                                                bool &should_exit);

    /// The 16 MCP tool definitions (name + description + inputSchema).
    [[nodiscard]] static nlohmann::json toolDefinitions();

  private:
    Backend m_backend;
};

} // namespace pulse::control
