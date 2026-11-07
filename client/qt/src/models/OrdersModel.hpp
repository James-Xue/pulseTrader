#pragma once

#include "models/JsonListModel.hpp"

namespace pulse::client
{

/// Active orders, keyed by `order_id`.
///
/// `get_orders.result.activeOrders` carries millisecond timestamps while
/// `recentReports` carries nanoseconds; that split is handled by the page, not
/// by this model.
class OrdersModel : public JsonListModel
{
    Q_OBJECT

public:
    explicit OrdersModel(QObject* parent = nullptr)
        : JsonListModel(QStringLiteral("order_id"), parent)
    {
    }
};

} // namespace pulse::client
