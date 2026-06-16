// order_tracker.cpp — OrderTracker implementation (Layer 8 Order Execution)

#include "pulse/execution/order_tracker.hpp"

#include "pulse/logging/logger.hpp"

namespace pulse::execution
{

using namespace pulse::logging;

OrderTracker::OrderTracker(exchange::GateWsClient &ws_client, exchange::GateRestClient &rest_client)
    : ws_client_{ ws_client }
    , rest_client_{ rest_client }
    , ws_subscribed_{ false }
{
}

void OrderTracker::track_order(const std::string &order_id,
    const Symbol &symbol,
    Side side,
    OrderType type,
    Quantity requested_qty,
    Price submit_mid_price)
{
    PULSE_LOG_INFO("execution", "Tracking order: {} {} {} {}", order_id, symbol, requested_qty,
        side == Side::Buy ? "buy" : "sell");

    // Subscribe to WS private channel if not already done
    if (!ws_subscribed_)
    {
        ws_client_.subscribe_private("spot.orders",
            {},
            [this](const nlohmann::json &result, const nlohmann::json & /*full_frame*/)
            { on_order_update(result); });
        ws_subscribed_ = true;
    }

    // Store tracked order metadata
    TrackedOrder order;
    order.order_id = order_id;
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
        std::unique_lock<std::shared_mutex> write_lock(mutex_);
        tracked_orders_[order_id] = order;
    }
}

void OrderTracker::stop_tracking(const std::string &order_id)
{
    PULSE_LOG_INFO("execution", "Stop tracking order: {}", order_id);

    std::unique_lock<std::shared_mutex> write_lock(mutex_);
    tracked_orders_.erase(order_id);
}

std::optional<OrderStatus> OrderTracker::get_status(const std::string &order_id) const
{
    std::shared_lock<std::shared_mutex> read_lock(mutex_);
    const auto it = tracked_orders_.find(order_id);
    if (it == tracked_orders_.end())
    {
        return std::nullopt;
    }
    return it->second.status;
}

std::optional<ExecutionReport> OrderTracker::get_report(const std::string &order_id) const
{
    std::shared_lock<std::shared_mutex> read_lock(mutex_);
    const auto it = completed_reports_.find(order_id);
    if (it == completed_reports_.end())
    {
        return std::nullopt;
    }
    return it->second;
}

void OrderTracker::set_completion_callback(CompletionCallback callback)
{
    completion_callback_ = std::move(callback);
}

Result<OrderStatus> OrderTracker::poll_order_status(const std::string &order_id)
{
    PULSE_LOG_DEBUG("execution", "Polling order status: {}", order_id);

    const std::string path = "/api/v4/spot/orders/" + order_id;
    auto result = rest_client_.request("GET", path);

    if (!ok(result))
    {
        return error(result);
    }

    const auto &resp = value(result);

    // Parse status
    const std::string status_str = resp.value("status", "");
    const OrderStatus new_status = parse_status(status_str);

    // Update tracked order
    {
        std::unique_lock<std::shared_mutex> write_lock(mutex_);
        auto it = tracked_orders_.find(order_id);
        if (it != tracked_orders_.end())
        {
            it->second.status = new_status;
            it->second.last_update_time = now();

            // Parse fill details if available
            if (resp.contains("filled_total"))
            {
                it->second.filled_qty = std::stod(resp["filled_total"].get<std::string>());
            }
            if (resp.contains("avg_deal_price"))
            {
                it->second.avg_fill_price = std::stod(resp["avg_deal_price"].get<std::string>());
            }
            if (resp.contains("fee"))
            {
                it->second.fees = std::stod(resp["fee"].get<std::string>());
            }

            // Check if terminal state
            if (is_terminal_status(new_status))
            {
                const ExecutionReport report = generate_report(it->second, now());
                completed_reports_[order_id] = report;

                if (completion_callback_)
                {
                    completion_callback_(report);
                }

                tracked_orders_.erase(it);
            }
        }
    }

    return new_status;
}

void OrderTracker::on_order_update(const nlohmann::json &event)
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

    process_order_update(event["result"]);
}

void OrderTracker::process_order_update(const nlohmann::json &update)
{
    if (!update.contains("id"))
    {
        return;
    }

    const std::string order_id = update["id"].get<std::string>();
    const std::string status_str = update.value("status", "");
    const OrderStatus new_status = parse_status(status_str);

    PULSE_LOG_DEBUG("execution", "Order update: {} -> {}", order_id, status_str);

    std::unique_lock<std::shared_mutex> write_lock(mutex_);
    auto it = tracked_orders_.find(order_id);
    if (it == tracked_orders_.end())
    {
        return; // Not tracking this order
    }

    // Update order state
    it->second.status = new_status;
    it->second.last_update_time = now();

    // Parse fill details
    if (update.contains("filled_total"))
    {
        it->second.filled_qty = std::stod(update["filled_total"].get<std::string>());
    }
    if (update.contains("avg_deal_price"))
    {
        it->second.avg_fill_price = std::stod(update["avg_deal_price"].get<std::string>());
    }
    if (update.contains("fee"))
    {
        it->second.fees = std::stod(update["fee"].get<std::string>());
    }

    // Check if terminal state
    if (is_terminal_status(new_status))
    {
        const ExecutionReport report = generate_report(it->second, now());
        completed_reports_[order_id] = report;

        PULSE_LOG_INFO("execution", "Order completed: {} {} filled_qty={} avg_price={} slippage={}bps",
            order_id, status_str, report.filled_qty, report.avg_fill_price, report.slippage_bps);

        if (completion_callback_)
        {
            completion_callback_(report);
        }

        tracked_orders_.erase(it);
    }
}

ExecutionReport OrderTracker::generate_report(const TrackedOrder &order, Timestamp fill_time) const
{
    ExecutionReport report;
    report.order_id = order.order_id;
    report.symbol = order.symbol;
    report.side = order.side;
    report.type = order.type;
    report.requested_qty = order.requested_qty;
    report.filled_qty = order.filled_qty;
    report.avg_fill_price = order.avg_fill_price;
    report.submit_mid_price = order.submit_mid_price;
    report.slippage_bps = ExecutionReport::calculate_slippage_bps(order.avg_fill_price, order.submit_mid_price, order.side);
    report.fees = order.fees;
    report.latency = std::chrono::duration_cast<std::chrono::milliseconds>(fill_time - order.submit_time);
    report.submit_time = order.submit_time;
    report.fill_time = fill_time;
    report.final_status = order.status;

    return report;
}

bool OrderTracker::is_terminal_status(OrderStatus status)
{
    return OrderStatus::Filled == status || OrderStatus::Cancelled == status;
}

OrderStatus OrderTracker::parse_status(const std::string &status_str)
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

} // namespace pulse::execution
