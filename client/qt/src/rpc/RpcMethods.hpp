#pragma once

#include <QString>

namespace pulse::client::methods
{

/// Control-plane method names. These are identical to the MCP tool names on
/// the engine side, so a rename here is a protocol break on both surfaces.
///
/// Only the six read-only methods are used by this client version; the
/// remaining names are listed so the transport's allow-list and the docs stay
/// in one place.
inline const QString kGetStatus = QStringLiteral("get_status");
inline const QString kGetAccount = QStringLiteral("get_account");
inline const QString kGetPositions = QStringLiteral("get_positions");
inline const QString kGetOrders = QStringLiteral("get_orders");
inline const QString kGetSignals = QStringLiteral("get_signals");
inline const QString kGetMarket = QStringLiteral("get_market");

} // namespace pulse::client::methods
