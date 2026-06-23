// order_tracker.cpp — OrderTracker implementation (Layer 8 Order Execution)

#include "execution/order_tracker.hpp"

#include "exchange/endpoint_router.hpp"
#include "logging/logger.hpp"

#include <algorithm>

namespace pulse::execution
{

using namespace pulse::logging;
using pulse::exchange::EndpointRouter;

OrderTracker::OrderTracker(exchange::GateWsClient &ws_client, exchange::GateRestClient &rest_client,
                           MarketType market_type)
    : m_wsClient{ ws_client }
    , m_restClient{ rest_client }
    , m_marketType{ market_type }
    , m_wsSubscribed{ false }
{
}

void OrderTracker::trackOrder(const std::string &order_id,
    const Symbol &symbol,
    Side side,
    OrderType type,
    Quantity requested_qty,
    Price submit_mid_price,
    const std::string &client_order_id)
{
    PULSE_LOG_INFO("execution", "Tracking order: {} {} {} {}", order_id, symbol, requested_qty,
        side == Side::Buy ? "buy" : "sell");

    // Subscribe to WS private channel if not already done
    if (!m_wsSubscribed)
    {
        const std::string orders_channel = EndpointRouter::wsChannel(m_marketType, "orders");
        m_wsClient.subscribePrivate(orders_channel,
            {},
            [this](const nlohmann::json &result, const nlohmann::json & /*full_frame*/)
            { onOrderUpdate(result); });
        m_wsSubscribed = true;
    }

    // Store tracked order metadata
    TrackedOrder order;
    order.order_id = order_id;
    order.client_order_id = client_order_id;
    order.symbol = symbol;
    order.side = side;
    order.type = type;
    order.requested_qty = requested_qty;
    order.submit_mid_price = submit_mid_price;
    order.filled_qty = 0.0;
    order.avg_fill_price = 0.0;
    order.fees = 0.0;
    order.status = OrderStatus::Pending;
    order.submit_time = now();
    order.last_update_time = order.submit_time;

    {
        std::unique_lock<std::shared_mutex> write_lock(m_mutex);
        m_trackedOrders[order_id] = order;
    }
}

void OrderTracker::stopTracking(const std::string &order_id)
{
    PULSE_LOG_INFO("execution", "Stop tracking order: {}", order_id);

    std::unique_lock<std::shared_mutex> write_lock(m_mutex);
    m_trackedOrders.erase(order_id);
}

std::optional<OrderStatus> OrderTracker::getStatus(const std::string &order_id) const
{
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    const auto it = m_trackedOrders.find(order_id);
    if (it == m_trackedOrders.end())
    {
        return std::nullopt;
    }
    return it->second.status;
}

std::optional<ExecutionReport> OrderTracker::getReport(const std::string &order_id) const
{
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    const auto it = m_completedReports.find(order_id);
    if (it == m_completedReports.end())
    {
        return std::nullopt;
    }
    return it->second;
}

void OrderTracker::setCompletionCallback(CompletionCallback callback)
{
    std::unique_lock<std::shared_mutex> write_lock(m_mutex);
    m_completionCallback = std::move(callback);
}

Result<OrderStatus> OrderTracker::pollOrderStatus(const std::string &order_id)
{
    PULSE_LOG_DEBUG("execution", "Polling order status: {}", order_id);

    const std::string path = EndpointRouter::orderPath(m_marketType, order_id);
    auto result = m_restClient.request("GET", path);

    if (!ok(result))
    {
        return error(result);
    }

    const auto &resp = value(result);

    // Parse status
    const std::string status_str = resp.value("status", "");
    const OrderStatus new_status = parseStatus(status_str);

    // Prepare callback data under lock, invoke outside lock to avoid
    // lock-ordering coupling with downstream mutexes (PositionManager, etc.)
    std::optional<ExecutionReport> completed_report;
    CompletionCallback callback_copy;

    // Update tracked order
    {
        std::unique_lock<std::shared_mutex> write_lock(m_mutex);
        auto it = m_trackedOrders.find(order_id);
        if (it != m_trackedOrders.end())
        {
            it->second.status = new_status;
            it->second.last_update_time = now();

            // Parse fill details if available
            if (resp.contains("filled_total"))
            {
                auto val = safeParseDouble(resp["filled_total"].get<std::string>());
                if (val.has_value()) it->second.filled_qty = val.value();
            }
            if (resp.contains("avg_deal_price"))
            {
                auto val = safeParseDouble(resp["avg_deal_price"].get<std::string>());
                if (val.has_value()) it->second.avg_fill_price = val.value();
            }
            if (resp.contains("fee"))
            {
                auto val = safeParseDouble(resp["fee"].get<std::string>());
                if (val.has_value()) it->second.fees = val.value();
            }

            // Check if terminal state — collect report + callback under lock
            if (isTerminalStatus(new_status))
            {
                completed_report = generateReport(it->second, now());
                m_completedReports[order_id] = *completed_report;

                callback_copy = m_completionCallback;
                m_trackedOrders.erase(it);
            }
        }
    } // write_lock released

    // Invoke callback outside lock — no lock-ordering coupling
    if (completed_report && callback_copy)
    {
        callback_copy(*completed_report);
    }

    return new_status;
}

void OrderTracker::onOrderUpdate(const nlohmann::json &event)
{
    // Gate.io spot.orders event format:
    // {
    //   "event": "update",
    //   "result": {
    //     "id": "12345",
    //     "status": "filled",
    //     "currency_pair": "BTC_USDT",
    //     "amount": "0.001",
    //     "filled_total": "0.001",
    //     "avg_deal_price": "50001",
    //     "fee": "0.05",
    //     ...
    //   }
    // }

    if (!event.contains("result"))
    {
        return;
    }

    processOrderUpdate(event["result"]);
}

void OrderTracker::processOrderUpdate(const nlohmann::json &update)
{
    if (!update.contains("id"))
    {
        return;
    }

    const std::string order_id = update["id"].get<std::string>();
    const std::string status_str = update.value("status", "");
    const OrderStatus new_status = parseStatus(status_str);

    PULSE_LOG_DEBUG("execution", "Order update: {} -> {}", order_id, status_str);

    // Prepare callback data under lock, invoke outside lock to avoid
    // lock-ordering coupling with downstream mutexes (PositionManager, etc.)
    std::optional<ExecutionReport> completed_report;
    CompletionCallback callback_copy;

    {
        std::unique_lock<std::shared_mutex> write_lock(m_mutex);
        auto it = m_trackedOrders.find(order_id);
        if (it == m_trackedOrders.end())
        {
            return; // Not tracking this order
        }

        // Update order state
        it->second.status = new_status;
        it->second.last_update_time = now();

        // Parse fill details
        if (update.contains("filled_total"))
        {
            auto val = safeParseDouble(update["filled_total"].get<std::string>());
            if (val.has_value()) it->second.filled_qty = val.value();
        }
        if (update.contains("avg_deal_price"))
        {
            auto val = safeParseDouble(update["avg_deal_price"].get<std::string>());
            if (val.has_value()) it->second.avg_fill_price = val.value();
        }
        if (update.contains("fee"))
        {
            auto val = safeParseDouble(update["fee"].get<std::string>());
            if (val.has_value()) it->second.fees = val.value();
        }

        // Check if terminal state — collect report + callback under lock
        if (isTerminalStatus(new_status))
        {
            completed_report = generateReport(it->second, now());
            m_completedReports[order_id] = *completed_report;

            PULSE_LOG_INFO("execution", "Order completed: {} {} filled_qty={} avg_price={} slippage={}bps",
                order_id, status_str, completed_report->filled_qty,
                completed_report->avg_fill_price, completed_report->slippage_bps);

            callback_copy = m_completionCallback;
            m_trackedOrders.erase(it);
        }
    } // write_lock released

    // Invoke callback outside lock — no lock-ordering coupling
    if (completed_report && callback_copy)
    {
        callback_copy(*completed_report);
    }
}

ExecutionReport OrderTracker::generateReport(const TrackedOrder &order, Timestamp fill_time) const
{
    ExecutionReport report;
    report.order_id = order.order_id;
    report.client_order_id = order.client_order_id;
    report.symbol = order.symbol;
    report.side = order.side;
    report.type = order.type;
    report.requested_qty = order.requested_qty;
    report.filled_qty = order.filled_qty;
    report.avg_fill_price = order.avg_fill_price;
    report.submit_mid_price = order.submit_mid_price;
    report.slippage_bps = ExecutionReport::calculateSlippageBps(order.avg_fill_price, order.submit_mid_price, order.side);
    report.fees = order.fees;
    report.latency = std::chrono::duration_cast<std::chrono::milliseconds>(fill_time - order.submit_time);
    report.submit_time = order.submit_time;
    report.fill_time = fill_time;
    report.final_status = order.status;

    return report;
}

bool OrderTracker::isTerminalStatus(OrderStatus status)
{
    return OrderStatus::Filled == status || OrderStatus::Cancelled == status;
}

OrderStatus OrderTracker::parseStatus(const std::string &status_str)
{
    if ("open" == status_str)
    {
        return OrderStatus::Open;
    }
    if ("closed" == status_str)
    {
        return OrderStatus::Filled;
    }
    if ("cancelled" == status_str)
    {
        return OrderStatus::Cancelled;
    }
    return OrderStatus::Pending;
}

std::vector<OrderSnapshot> OrderTracker::activeOrders() const
{
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    std::vector<OrderSnapshot> result;
    result.reserve(m_trackedOrders.size());
    for (const auto &[id, order] : m_trackedOrders)
    {
        if (!isTerminalStatus(order.status))
        {
            OrderSnapshot snap;
            snap.order_id = order.order_id;
            snap.symbol = order.symbol;
            snap.side = order.side;
            snap.type = order.type;
            snap.requested_qty = order.requested_qty;
            snap.filled_qty = order.filled_qty;
            snap.status = order.status;
            snap.submit_time = order.submit_time;
            snap.last_update_time = order.last_update_time;
            result.push_back(std::move(snap));
        }
    }
    return result;
}

std::vector<ExecutionReport> OrderTracker::recentReports(std::size_t n) const
{
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    std::vector<ExecutionReport> result;
    result.reserve(m_completedReports.size());
    for (const auto &[id, report] : m_completedReports)
    {
        result.push_back(report);
    }

    // Sort by fill_time descending (most recent first).
    std::sort(result.begin(), result.end(),
        [](const ExecutionReport &a, const ExecutionReport &b)
        {
            return a.fill_time > b.fill_time;
        });

    if (result.size() > n)
    {
        result.resize(n);
    }
    return result;
}

} // namespace pulse::execution
