#pragma once

#include <QString>

namespace pulse::client::methods
{

/// Control-plane method names. These are identical to the MCP tool names on
/// the engine side, so a rename here is a protocol break on both surfaces.
///
/// Only read-only methods are used by this client version; the remaining
/// names are listed so the transport's allow-list and the docs stay in one
/// place.
inline const QString kGetStatus = QStringLiteral("get_status");
inline const QString kGetAccount = QStringLiteral("get_account");
inline const QString kGetPositions = QStringLiteral("get_positions");
inline const QString kGetOrders = QStringLiteral("get_orders");
inline const QString kGetSignals = QStringLiteral("get_signals");
inline const QString kGetMarket = QStringLiteral("get_market");

/// Gold Traders page (read-only). `list_strategies` returns a top-level array;
/// the other two return objects.
inline const QString kListStrategies = QStringLiteral("list_strategies");
inline const QString kGetStrategyParams = QStringLiteral("get_strategy_params");
inline const QString kGetRisk = QStringLiteral("get_risk");

} // namespace pulse::client::methods
