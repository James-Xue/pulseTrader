#pragma once

#include "models/JsonListModel.hpp"

namespace pulse::client
{

/// Recent execution reports, keyed by `order_id`.
///
/// One order can contribute several fills, so duplicates are expected; the
/// keyed update path compares the whole sequence and tolerates them. Fill times
/// are nanoseconds in this payload.
class ReportsModel : public JsonListModel
{
    Q_OBJECT

public:
    explicit ReportsModel(QObject* parent = nullptr)
        : JsonListModel(QStringLiteral("order_id"), parent)
    {
    }
};

} // namespace pulse::client
