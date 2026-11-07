#pragma once

#include "models/JsonListModel.hpp"

namespace pulse::client
{

/// Open positions, keyed by `position_id`.
///
/// Roles are the 19 serialized position fields plus the engine's `*_str`
/// renderings; they are discovered from the payload, so a new engine field
/// reaches QML without a client change.
class PositionsModel : public JsonListModel
{
    Q_OBJECT

public:
    explicit PositionsModel(QObject* parent = nullptr)
        : JsonListModel(QStringLiteral("position_id"), parent)
    {
    }
};

} // namespace pulse::client
