#pragma once

#include <QString>

namespace pulse::client
{

/// Classification of a failed control-plane call.
///
/// The UI maps each kind to a different affordance: transport and TLS failures
/// offer "reconnect", timeouts offer "retry", auth failures open the
/// credentials dialog, and RpcError kinds surface the engine's own message.
enum class RpcErrorKind
{
    Transport,      ///< Socket never connected, or the peer closed it.
    TlsHandshake,   ///< TLS negotiation failed (certificate, protocol, or timeout).
    Timeout,        ///< The request was written but no reply arrived in budget.
    AuthFailed,     ///< The engine rejected the token.
    RpcError,       ///< A well-formed JSON-RPC error object (code != 0).
    InBand,         ///< HTTP 200-shaped result carrying a logical "error" string.
    ProtocolDesync, ///< Unparseable line, unknown reply id, or frame too large.
};

/// One failed call, carrying enough context for the UI to explain itself.
///
/// jsonRpcCode and dataCode are separate on purpose: the engine returns the
/// JSON-RPC code in `error.code` and the PulseError value in `error.data.code`
/// (for example -32603 plus 9103). Both are preserved verbatim.
struct RpcError
{
    RpcErrorKind kind = RpcErrorKind::Transport;
    QString message;
    int jsonRpcCode = 0;
    int dataCode = 0;

    /// Human-readable one-liner: "<kind>: <message> [rpc=<code> data=<code>]".
    [[nodiscard]] QString toString() const;
};

/// Lowercase stable name for a kind, used in logs and QML tooltips.
[[nodiscard]] QString toString(RpcErrorKind kind);

} // namespace pulse::client
