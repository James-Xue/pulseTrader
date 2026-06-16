// order_executor.cpp — OrderExecutor implementation (Layer 8 Order Execution)

#include "pulse/execution/order_executor.hpp"

#include "pulse/logging/logger.hpp"

#include <charconv>

namespace pulse::execution
{

using namespace pulse::logging;

OrderExecutor::OrderExecutor(exchange::GateRestClient &rest_client)
    : rest_client_{ rest_client }
{
}

Result<OrderResponse> OrderExecutor::place_order(const OrderRequest &req)
{
    PULSE_LOG_INFO("execution",
        "Placing {} {} order: {} {} @ {}",
        req.symbol,
        req.side == Side::Buy ? "buy" : "sell",
        req.type == OrderType::Market   ? "market"
        : req.type == OrderType::Limit  ? "limit"
        : req.type == OrderType::PostOnly ? "post_only"
                                          : "unknown",
        req.quantity,
        req.price);

    // Build order body
    const nlohmann::json body_json = build_order_body(req);
    const std::string body = body_json.dump();

    // Submit order via REST (retry logic is in GateRestClient::request)
    auto result = rest_client_.request("POST", "/api/v4/spot/orders", "", body);

    if (!ok(result))
    {
        PULSE_LOG_ERROR("execution", "Failed to place order: {}", error(result).message);
        return error(result);
    }

    // Parse response
    const auto &resp_json = value(result);
    const OrderResponse resp = parse_order_response(resp_json);

    PULSE_LOG_INFO("execution", "Order placed: id={}, status={}", resp.order_id, resp_json.value("status", "unknown"));

    return resp;
}

bool OrderExecutor::cancel_order(const std::string &order_id)
{
    PULSE_LOG_INFO("execution", "Cancelling order: {}", order_id);

    const std::string path = "/api/v4/spot/orders/" + order_id;
    auto result = rest_client_.request("DELETE", path);

    if (!ok(result))
    {
        PULSE_LOG_ERROR("execution", "Failed to cancel order {}: {}", order_id, error(result).message);
        return false;
    }

    PULSE_LOG_INFO("execution", "Order cancelled: {}", order_id);
    return true;
}

nlohmann::json OrderExecutor::build_order_body(const OrderRequest &req) const
{
    nlohmann::json body;
    body["currency_pair"] = req.symbol;
    body["side"] = (Side::Buy == req.side) ? "buy" : "sell";

    // Order type
    switch (req.type)
    {
    case OrderType::Market:
        body["type"] = "market";
        break;
    case OrderType::Limit:
        body["type"] = "limit";
        body["price"] = std::to_string(req.price);
        body["time_in_force"] = "gtc"; // Good-til-cancelled
        break;
    case OrderType::PostOnly:
        body["type"] = "limit";
        body["price"] = std::to_string(req.price);
        body["time_in_force"] = "poc"; // Post-only-cancel
        break;
    }

    // Quantity (Gate.io uses "amount" for base currency quantity)
    body["amount"] = std::to_string(req.quantity);

    // Optional client order ID
    if (!req.client_order_id.empty())
    {
        body["text"] = "t-" + req.client_order_id; // Gate.io prefix: "t-"
    }

    return body;
}

OrderResponse OrderExecutor::parse_order_response(const nlohmann::json &resp) const
{
    OrderResponse result;

    // Order ID
    if (resp.contains("id"))
    {
        result.order_id = resp["id"].get<std::string>();
    }

    // Status
    if (resp.contains("status"))
    {
        const std::string status_str = resp["status"].get<std::string>();
        if ("open" == status_str)
        {
            result.status = OrderStatus::Open;
        }
        else if ("closed" == status_str)
        {
            result.status = OrderStatus::Filled;
        }
        else if ("cancelled" == status_str)
        {
            result.status = OrderStatus::Cancelled;
        }
        else
        {
            result.status = OrderStatus::Pending;
        }
    }

    // Submit time
    if (resp.contains("create_time"))
    {
        const std::int64_t create_time_ms = resp["create_time"].get<std::int64_t>() * 1000; // Gate.io uses seconds
        result.submit_time = Timestamp{ std::chrono::milliseconds{ create_time_ms } };
    }
    else
    {
        result.submit_time = now();
    }

    return result;
}

} // namespace pulse::execution
