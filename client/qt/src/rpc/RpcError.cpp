#include "rpc/RpcError.hpp"

namespace pulse::client
{

QString toString(RpcErrorKind kind)
{
    // Exhaustive switch with no default: adding a kind becomes a compile error
    // instead of a silently unlabelled value.
    switch (kind)
    {
    case RpcErrorKind::Transport:
        return QStringLiteral("transport");
    case RpcErrorKind::TlsHandshake:
        return QStringLiteral("tls");
    case RpcErrorKind::Timeout:
        return QStringLiteral("timeout");
    case RpcErrorKind::AuthFailed:
        return QStringLiteral("auth");
    case RpcErrorKind::RpcError:
        return QStringLiteral("rpc");
    case RpcErrorKind::InBand:
        return QStringLiteral("in-band");
    case RpcErrorKind::ProtocolDesync:
        return QStringLiteral("desync");
    }
    return QStringLiteral("unknown");
}

QString RpcError::toString() const
{
    // 1. Start from the stable kind name so logs stay greppable.
    // 2. Append the human message when the transport supplied one.
    // 3. Append the numeric codes only when they carry information; zero means
    //    "not applicable", which would otherwise add noise to every line.
    QString text = pulse::client::toString(kind);
    if (!message.isEmpty())
    {
        text += QStringLiteral(": ") + message;
    }
    if (0 != jsonRpcCode || 0 != dataCode)
    {
        text += QStringLiteral(" [rpc=%1 data=%2]").arg(jsonRpcCode).arg(dataCode);
    }
    return text;
}

} // namespace pulse::client
