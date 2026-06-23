// order_executor.cpp — OrderExecutor implementation (Layer 8 Order Execution)

#include "execution/OrderExecutor.hpp"

#include "exchange/EndpointRouter.hpp"
#include "logging/Logger.hpp"

#include <charconv>

namespace pulse::execution
{

using namespace pulse::logging;
using pulse::exchange::EndpointRouter;

OrderExecutor::OrderExecutor(exchange::GateRestClient &rest_client, MarketType market_type)
    : m_restClient{ rest_client }
    , m_marketType{ market_type }
{
}

Result<OrderResponse> OrderExecutor::placeOrder(const OrderRequest &req)
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
    const nlohmann::json body_json = buildOrderBody(req);
    const std::string body = body_json.dump();

    // Submit order via REST (retry logic is in GateRestClient::request)
    auto result = m_restClient.request("POST", EndpointRouter::ordersPath(m_marketType), "", body);

    if (!ok(result))
    {
        PULSE_LOG_ERROR("execution", "Failed to place order: {}", error(result).message);
        return error(result);
    }

    // Parse response
    const auto &resp_json = value(result);
    const OrderResponse resp = parseOrderResponse(resp_json);

    PULSE_LOG_INFO("execution", "Order placed: id={}, status={}", resp.order_id, resp_json.value("status", "unknown"));

    return resp;
}

bool OrderExecutor::cancelOrder(const std::string &order_id)
{
    PULSE_LOG_INFO("execution", "Cancelling order: {}", order_id);

    const std::string path = EndpointRouter::orderPath(m_marketType, order_id);
    auto result = m_restClient.request("DELETE", path);

    if (!ok(result))
    {
        PULSE_LOG_ERROR("execution", "Failed to cancel order {}: {}", order_id, error(result).message);
        return false;
    }

    PULSE_LOG_INFO("execution", "Order cancelled: {}", order_id);
    return true;
}

nlohmann::json OrderExecutor::buildOrderBody(const OrderRequest &req) const
{
    nlohmann::json body;

    if (MarketType::Futures == m_marketType)
    {
        // --- Futures order format ---
        body["contract"] = req.symbol;

        // Size: positive = buy/long, negative = sell/short (in contracts).
        int size = req.contract_size;
        if (0 == size)
        {
            // If contract_size not set, use quantity as contract count (rounded).
            size = static_cast<int>(std::round(req.quantity));
        }
        body["size"] = (Side::Sell == req.side) ? -size : size;

        // Price: "0" for market orders (with tif=ioc), actual price for limit.
        if (OrderType::Market == req.type)
        {
            body["price"] = "0";
            body["tif"] = "ioc"; // Immediate-or-cancel for market orders.
        }
        else
        {
            body["price"] = std::to_string(req.price);
            body["tif"] = (OrderType::PostOnly == req.type) ? "poc" : "gtc";
        }

        body["reduce_only"] = req.reduce_only;
    }
    else
    {
        // --- Spot order format (unchanged) ---
        body["currency_pair"] = req.symbol;
        body["side"] = (Side::Buy == req.side) ? "buy" : "sell";

        switch (req.type)
        {
        case OrderType::Market:
            body["type"] = "market";
            break;
        case OrderType::Limit:
            body["type"] = "limit";
            body["price"] = std::to_string(req.price);
            body["time_in_force"] = "gtc";
            break;
        case OrderType::PostOnly:
            body["type"] = "limit";
            body["price"] = std::to_string(req.price);
            body["time_in_force"] = "poc";
            break;
        }

        body["amount"] = std::to_string(req.quantity);
    }

    // Optional client order ID (same format for both markets).
    if (!req.client_order_id.empty())
    {
        body["text"] = "t-" + req.client_order_id; // Gate.io prefix: "t-"
    }

    return body;
}

OrderResponse OrderExecutor::parseOrderResponse(const nlohmann::json &resp) const
{
    OrderResponse result;

    // Order ID — spot returns string, futures returns integer.
    if (resp.contains("id"))
    {
        if (resp["id"].is_number())
        {
            result.order_id = std::to_string(resp["id"].get<std::int64_t>());
        }
        else
        {
            result.order_id = resp["id"].get<std::string>();
        }
    }

    // Status — spot uses "status" (open/closed/cancelled),
    // futures uses "status" (open/finished) + "finish_as" (filled/cancelled/etc).
    if (MarketType::Futures == m_marketType && resp.contains("finish_as"))
    {
        const std::string finish_as = resp["finish_as"].get<std::string>();
        if ("filled" == finish_as)
        {
            result.status = OrderStatus::Filled;
        }
        else if ("cancelled" == finish_as || "reduce_only" == finish_as
                 || "position_closed" == finish_as)
        {
            result.status = OrderStatus::Cancelled;
        }
        else
        {
            result.status = OrderStatus::Open;
        }
    }
    else if (resp.contains("status"))
    {
        const std::string status_str = resp["status"].get<std::string>();
        if ("open" == status_str)
        {
            result.status = OrderStatus::Open;
        }
        else if ("closed" == status_str || "finished" == status_str)
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

    // Submit time — futures uses float (with fractional seconds), spot uses int.
    if (resp.contains("create_time"))
    {
        if (resp["create_time"].is_number_float())
        {
            // Futures: float seconds (e.g. 1700000000.123)
            const double create_time_sec = resp["create_time"].get<double>();
            result.submit_time = Timestamp{ std::chrono::milliseconds{
                static_cast<std::int64_t>(create_time_sec * 1000) } };
        }
        else
        {
            // Spot: integer seconds
            const std::int64_t create_time_ms = resp["create_time"].get<std::int64_t>() * 1000;
            result.submit_time = Timestamp{ std::chrono::milliseconds{ create_time_ms } };
        }
    }
    else
    {
        result.submit_time = now();
    }

    return result;
}

} // namespace pulse::execution
