// order_executor.cpp — OrderExecutor implementation (Layer 8 Order Execution)

#include "execution/OrderExecutor.hpp"

#include "exchange/EndpointRouter.hpp"
#include "logging/Logger.hpp"

#include <charconv>
#include <cmath>

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
        : req.type == OrderType::MakerFirst ? "maker_first"
                                          : "unknown",
        req.quantity,
        req.price);

    // Anchor the CFD match window (Unix seconds) BEFORE the POST — the
    // exchange's time_setup must not predate the request.
    const std::int64_t placed_at_unix =
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();

    // Build order body
    const nlohmann::json body_json = buildOrderBody(m_marketType, req);
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
    OrderResponse resp = parseOrderResponse(resp_json);

    // TradFi CFD: the POST may carry a 2xx business error ({"label","message"}
    // body) that the HTTP status check cannot see — surface it as a real
    // failure so the caller releases its risk reservation.
    if (MarketType::Cfd == m_marketType)
    {
        if (auto biz_err = cfdBusinessError(resp_json))
        {
            PULSE_LOG_ERROR("execution", "CFD order rejected by exchange: {}",
                            biz_err->message);
            return *biz_err;
        }
    }

    // TradFi CFD: POST /tradfi/orders does not echo the exchange order id
    // (data.id is an internal submission number) — resolve it from the
    // open-orders list, matched by symbol/side/volume/price within the
    // placed_at window (stale same-key leftovers are never matched).
    if (MarketType::Cfd == m_marketType && resp.order_id.empty())
    {
        auto list_result = m_restClient.request(
            "GET", EndpointRouter::ordersPath(MarketType::Cfd));
        if (ok(list_result))
        {
            const auto &data =
                value(list_result).value("data", nlohmann::json::object());
            resp.order_id = matchCfdOrderId(
                data.value("list", nlohmann::json::array()), req, placed_at_unix);
            if (!resp.order_id.empty())
            {
                resp.status = OrderStatus::Open;
                PULSE_LOG_INFO("execution",
                    "Resolved CFD order id {} from the open-orders list",
                    resp.order_id);
            }
            else
            {
                PULSE_LOG_WARN("execution",
                    "CFD order placed but id not resolvable from the "
                    "open-orders list (matched symbol={} side={} volume={})",
                    req.symbol, req.side == Side::Buy ? "buy" : "sell",
                    req.quantity);
            }
        }
        else
        {
            PULSE_LOG_WARN("execution",
                "CFD order placed but order-list query failed: {}",
                error(list_result).message);
        }
    }

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

Result<nlohmann::json> OrderExecutor::setLeverage(const std::string &contract,
                                                  double leverage)
{
    // Cache hit: this contract already runs at the requested leverage —
    // skip the API call (orders fire every ~500ms per strategy).
    {
        std::lock_guard lock(m_leverageMutex);
        const auto it = m_leverageCache.find(contract);
        if (it != m_leverageCache.end() && it->second == leverage)
        {
            return nlohmann::json{ { "ok", true } };
        }
    }

    auto result = m_restClient.setFuturesLeverage(contract, leverage);
    if (!ok(result))
    {
        PULSE_LOG_ERROR("execution", "Failed to set {} leverage to {}x: {}",
            contract, leverage, error(result).message);
        return result;
    }

    {
        std::lock_guard lock(m_leverageMutex);
        m_leverageCache[contract] = leverage;
    }
    PULSE_LOG_INFO("execution", "Futures leverage set: {} {}x", contract, leverage);
    return result;
}

nlohmann::json OrderExecutor::buildOrderBody(MarketType mt, const OrderRequest &req)
{
    nlohmann::json body;

    switch (mt)
    {
    case MarketType::Futures:
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
            // Defensive: requests never carry MakerFirst (buildRequestFromSignal
            // maps it to PostOnly), but a leaked value must not silently become
            // a GTC limit order.
            body["tif"] = (OrderType::PostOnly == req.type
                           || OrderType::MakerFirst == req.type)
                              ? "poc"
                              : "gtc";
        }

        body["reduce_only"] = req.reduce_only;
        break;
    }
    case MarketType::Cfd:
    {
        // --- TradFi CFD order format (MT5 style) ---
        // side: 2 = buy, 1 = sell; volume in lots (0.01 min for XAUUSD).
        //
        // CFD cost model (recorded, NOT enforced by the engine):
        //   - Commission: 0.06 USDT per 0.01 lot, charged once on BUY only
        //     (sell side is free); 6 USDT per full lot in the symbol detail.
        //   - Gold storage/swap (利差): charged per overnight hold, surfaced
        //     in the assets `storage` field. Long positions typically pay.
        body["symbol"] = req.symbol;
        body["side"] = (Side::Buy == req.side) ? 2 : 1;
        body["volume"] = std::to_string(req.quantity);

        // price_type: "market" for market orders, "trigger" for limit orders.
        body["price_type"] = (OrderType::Market == req.type) ? "market" : "trigger";
        if (OrderType::Market != req.type)
        {
            body["price"] = std::to_string(req.price);
        }
        // price_tp / price_sl (take-profit / stop-loss per order) are optional —
        // left empty for now; the engine's own stop-loss engine covers exits.
        break;
    }
    default:
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
        case OrderType::MakerFirst:   // Defensive: never in flight (mapped to PostOnly).
            body["type"] = "limit";
            body["price"] = std::to_string(req.price);
            body["time_in_force"] = "poc";
            break;
        }

        body["amount"] = std::to_string(req.quantity);
        break;
    }
    }

    // Optional client order ID (spot/futures only — the MT5 CFD schema has no text field).
    if (!req.client_order_id.empty() && MarketType::Cfd != mt)
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
    // TradFi CFD wraps the payload in {"data": ...}; data.id is an internal
    // submission reference (probe-verified 2026-08-17: string, e.g. "43713").
    // Use it as the tracking key when the list-based resolution is empty.
    if (result.order_id.empty() && resp.contains("data")
        && resp["data"].is_object() && resp["data"].contains("id"))
    {
        const auto &inner_id = resp["data"]["id"];
        if (inner_id.is_string())
        {
            result.order_id = inner_id.get<std::string>();
        }
        else if (inner_id.is_number())
        {
            result.order_id = std::to_string(inner_id.get<std::int64_t>());
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

std::string OrderExecutor::matchCfdOrderId(const nlohmann::json &list,
                                           const OrderRequest &req,
                                           std::int64_t placed_at_unix)
{
    // The TradFi orders list is newest-first; the first entry matching the
    // placed request is ours. The list echoes volumes/prices as strings with
    // the exchange's own precision (e.g. "0.01", "3000.00"), so compare
    // numerically rather than string-wise.
    const int side = (Side::Buy == req.side) ? 2 : 1;
    constexpr std::int64_t kCfdMatchSlackSec = 5; // Covers clock skew.

    for (const auto &o : list)
    {
        if (o.value("symbol", "") != req.symbol)
        {
            continue;
        }
        if (o.value("side", 0) != side)
        {
            continue;
        }

        const double volume =
            safeParseDouble(o.value("volume", "0")).value_or(-1.0);
        if (volume < 0.0 || std::abs(volume - req.quantity) > 1e-9)
        {
            continue;
        }

        // Only orders placed AFTER our POST can be ours — a filled market
        // order leaves the list immediately, so without this window a stale
        // same-key legacy trigger (e.g. an old buy@4295 for a new buy 0.01)
        // would be matched and later cancelled by mistake (2026-08-17
        // incident). time_setup=0 (missing) is always outside the window.
        const std::int64_t time_setup = o.value("time_setup", 0LL);
        if (time_setup < placed_at_unix - kCfdMatchSlackSec)
        {
            continue;
        }

        // Trigger orders must match the price too; market orders match on
        // symbol/side/volume only.
        if (OrderType::Market != req.type)
        {
            const double price =
                safeParseDouble(o.value("price", "0")).value_or(-1.0);
            if (price < 0.0 || std::abs(price - req.price) > 1e-6)
            {
                continue;
            }
        }

        if (o.contains("order_id"))
        {
            if (o["order_id"].is_number())
            {
                return std::to_string(o["order_id"].get<std::int64_t>());
            }
            return o["order_id"].get<std::string>();
        }
        // order_id missing — try "id" as a fallback field name.
        if (o.contains("id"))
        {
            if (o["id"].is_number())
            {
                return std::to_string(o["id"].get<std::int64_t>());
            }
            return o["id"].get<std::string>();
        }
    }

    return {};
}

std::optional<PulseError> OrderExecutor::cfdBusinessError(const nlohmann::json &resp)
{
    // Success bodies are wrapped as {"data": {...}}; business errors come back
    // as {"label": "...", "message": "...", "data": null} with a 2xx HTTP
    // status (probe-verified 2026-08-17: data is present but null).
    if (resp.is_object() && (resp.contains("label") || resp.contains("message")))
    {
        const bool has_data = resp.contains("data") && !resp["data"].is_null();
        if (!has_data)
        {
            const std::string label = resp.value("label", "");
            const std::string message = resp.value("message", "");
            return PulseError{ ErrorCode::OrderRejected,
                               "CFD order rejected by exchange: " + label + ": " + message };
        }
    }
    return std::nullopt;
}

} // namespace pulse::execution
