#pragma once

#include "models/JsonListModel.hpp"

namespace pulse::client
{

/// Latest strategy signal per source, keyed by `source`.
///
/// The engine keeps only the newest entry per strategy, so this list is short
/// and its order is the engine's map order, not a ranking.
class SignalsModel : public JsonListModel
{
    Q_OBJECT

public:
    explicit SignalsModel(QObject* parent = nullptr)
        : JsonListModel(QStringLiteral("source"), parent)
    {
    }
};

} // namespace pulse::client
