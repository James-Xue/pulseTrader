#pragma once
// McpServer.hpp — stdio MCP (Model Context Protocol) server, dual-era
//
// Speaks the MCP 2025 stdio transport: newline-delimited JSON-RPC 2.0 on
// stdin/stdout. Tools are the control-plane methods; each tools/call is
// forwarded to the engine's control socket via the Backend.
//
// Stdio hygiene (critical): the host process must route spdlog console
// output AWAY from stdout (mcp mode forces toConsole=false) and must never
// write to std::cout — the protocol stream is the only thing on stdout.
//
// ---------------------------------------------------------------------------
// Dual-era support (2026-10-09)
// ---------------------------------------------------------------------------
// MCP revision 2026-07-28 is the largest breaking change since 2024-11-05. It
// removed the initialize handshake, removed protocol-level sessions, removed
// ping, and made the core stateless: each request declares its version, client
// identity and capabilities in `_meta`. It also requires servers to implement
// `server/discover`, and requires every RESULT to carry `resultType`.
//
// This server previously answered only the legacy handshake and hard-coded one
// version, which works today only because the client happens to negotiate
// down. A client that moves to the modern era would have found every one of the
// trading tools missing, so both eras are supported now:
//   1. `server/discover` answers with the full version list -> modern clients
//      proceed with no handshake.
//   2. `initialize` still negotiates the legacy set -> legacy clients work
//      exactly as before.
//   3. tools/list and tools/call are served whether or not a handshake
//      happened; nothing is gated on session state, which is what lets (1) and
//      (2) coexist and is true to the modern stateless model.
//
// Wire shapes taken from @modelcontextprotocol/server 2.0.0 and confirmed
// against a live client. Two details are easy to get wrong and SILENT when
// wrong — a client that cannot parse either reply classifies the server as
// legacy and never says why:
//   - the discover result key is `supportedVersions`, NOT `protocolVersions`;
//   - the -32022 error data is `{supported, requested}`.
// A third produces a connected server with NO tools at all: a modern result
// without `resultType` is rejected outright.
//
// The handleLine signature is deliberately unchanged — the parameter plumbing
// lives inside it — so this addition cannot disturb the existing tests or the
// `mcp` subcommand's wiring.

#include "control/JsonRpcServer.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <iostream>
#include <string>
#include <vector>

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

    /// The control-plane methods as MCP tool definitions
    /// (name + description + inputSchema).
    [[nodiscard]] static nlohmann::json toolDefinitions();

    /// Legacy (handshake-era) revisions, ascending, newest last:
    /// 2024-10-07, 2024-11-05, 2025-03-26, 2025-06-18, 2025-11-25.
    ///
    /// `initialize` negotiates only within this list — a server MUST NOT answer
    /// a legacy handshake with a modern revision, because a legacy client
    /// cannot parse a modern session.
    [[nodiscard]] static const std::vector<std::string> &legacyProtocolVersions();

    /// Newest legacy revision: the cap for a legacy `initialize` reply and the
    /// counter-offer when the client asks for something unrecognised.
    [[nodiscard]] static const std::string &newestLegacyProtocolVersion();

    /// Every revision this server speaks: modern first, then legacy
    /// descending. Advertised by `server/discover` and by -32022.
    [[nodiscard]] static const std::vector<std::string> &supportedProtocolVersions();

    /// Select the revision to answer a legacy `initialize` with:
    ///   1. the client's, when it is a supported legacy revision (the spec
    ///      requires echoing a supported version verbatim);
    ///   2. otherwise the newest legacy revision, as the counter-offer. That is
    ///      not an error; the client decides whether to continue.
    [[nodiscard]] static std::string negotiateLegacyVersion(const std::string &requested);

    /// The revision a modern request declared in `_meta`, or "" when the
    /// request carries no version metadata (a legacy client, or a modern client
    /// that omits it). The key is
    /// `io.modelcontextprotocol/protocolVersion`.
    [[nodiscard]] static std::string declaredProtocolVersion(const nlohmann::json &params);

  private:
    Backend m_backend;
};

/// JSON-RPC code for "unsupported protocol version", introduced by MCP
/// 2026-07-28. Its data carries the supported list so the client can pick a
/// mutually supported revision and retry.
inline constexpr int kErrorUnsupportedProtocolVersion{ -32022 };

} // namespace pulse::control
