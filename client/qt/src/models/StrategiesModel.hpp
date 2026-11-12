#pragma once

#include "models/JsonListModel.hpp"

namespace pulse::client
{

/// One row per registered strategy instance, keyed by `id`.
///
/// The `list_strategies` payload is a top-level JSON array (the only read-only
/// method that is not an object). Roles are the eight keys the engine
/// serializes from `StrategySnapshot` — id, name, symbol, enabled, running,
/// paused, auto_trade, poll_interval_ms — and are discovered from the first
/// observed row, so delegates must guard `model.<key> === undefined`.
///
/// Keying on `id` (e.g. `momentum_scalper_XAUUSD`) means an unchanged instance
/// list updates in place instead of resetting the model, which keeps the
/// selected row stable across polls.
class StrategiesModel : public JsonListModel
{
    Q_OBJECT

public:
    explicit StrategiesModel(QObject* parent = nullptr)
        : JsonListModel(QStringLiteral("id"), parent)
    {
    }
};

} // namespace pulse::client