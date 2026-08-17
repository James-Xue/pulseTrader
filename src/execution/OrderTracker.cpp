// order_tracker.cpp — OrderTracker implementation (Layer 8 Order Execution)

#include "execution/OrderTracker.hpp"

#include "exchange/EndpointRouter.hpp"
#include "logging/Logger.hpp"

#include <algorithm>
#include <cmath>

namespace pulse::execution
{

namespace
{

/// Parse a JSON number-or-string field (Gate.io mixes both types across
/// spot/futures responses — futures uses numeric size/left, spot uses
/// string-typed filled_total/avg_deal_price).
double jsonNumOrStr(const nlohmann::json &update, const char *key, double fallback)
{
    if (!update.contains(key))
    {
        return fallback;
    }
    const auto &v = update[key];
    if (v.is_number())
    {
        return v.get<double>();
    }
    if (v.is_string())
    {
        try
        {
            return std::stod(v.get<std::string>());
        }
        catch (const std::exception &)
        {
            return fallback;
        }
    }
    return fallback;
}

/// Normalize an order id — futures ids are integers, spot ids are strings.
std::string orderIdToString(const nlohmann::json &id_value)
{
    if (id_value.is_number())
    {
        return std::to_string(id_value.get<std::int64_t>());
    }
    return id_value.get<std::string>();
}

} // anonymous namespace

using namespace pulse::logging;
using pulse::exchange::EndpointRouter;

OrderTracker::OrderTracker(exchange::GateWsClient *ws_client, exchange::GateRestClient &rest_client,
                           MarketType market_type, bool enable_ws)
    : m_wsClient{ ws_client }
    , m_restClient{ rest_client }
    , m_marketType{ market_type }
    , m_wsSubscribed{ false }
    , m_enableWs{ enable_ws }
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

    // Subscribe to WS private channel if not already done (skipped for
    // REST-poll-only markets like TradFi CFD — no private WS channel exists).
    if (!m_wsSubscribed && m_enableWs && nullptr != m_wsClient)
    {
        const std::string orders_channel = EndpointRouter::wsChannel(m_marketType, "orders");
        m_wsClient->subscribePrivate(orders_channel,
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

    // TradFi CFD: the single-order GET /tradfi/orders/{id} endpoint does not
    // exist (route-level 404) — status comes from the open-orders list with
    // a positions-based fill fallback instead.
    if (MarketType::Cfd == m_marketType)
    {
        return pollCfdOrderStatus(order_id);
    }

    const std::string path = EndpointRouter::orderPath(m_marketType, order_id);
    auto result = m_restClient.request("GET", path);

    if (!ok(result))
    {
        return error(result);
    }

    const auto &resp = value(result);

    // Prepare callback data under lock, invoke outside lock to avoid
    // lock-ordering coupling with downstream mutexes (PositionManager, etc.)
    std::optional<ExecutionReport> completed_report;
    CompletionCallback callback_copy;

    // Update tracked order
    OrderStatus new_status = OrderStatus::Pending;
    {
        std::unique_lock<std::shared_mutex> write_lock(m_mutex);
        auto it = m_trackedOrders.find(order_id);
        if (it != m_trackedOrders.end())
        {
            completed_report = applyUpdateAndMaybeComplete(order_id, resp);
            if (completed_report)
            {
                new_status = completed_report->final_status;
            }
            else
            {
                // Still tracked (non-terminal) — the entry survives.
                new_status = it->second.status;
            }
            callback_copy = m_completionCallback;
        }
        else
        {
            // Not tracked (e.g. reconciled after removal) — derive from
            // the response alone so callers still get a correct status.
            new_status = parseFuturesStatus(resp.value("status", ""),
                                            resp.value("finish_as", ""));
        }
    } // write_lock released

    // Invoke callback outside lock — no lock-ordering coupling
    if (completed_report && callback_copy)
    {
        callback_copy(*completed_report);
    }

    return new_status;
}

Result<OrderStatus> OrderTracker::pollCfdOrderStatus(const std::string &order_id)
{
    auto list_result = m_restClient.getCfdOrders();
    if (!ok(list_result))
    {
        return error(list_result);
    }

    const auto &data = value(list_result).value("data", nlohmann::json::object());
    const auto list = data.value("list", nlohmann::json::array());
    auto found = findCfdOrderInList(list, order_id);

    // Prepare callback data under lock, invoke outside lock.
    std::optional<ExecutionReport> completed_report;
    CompletionCallback callback_copy;

    if (!found.is_null())
    {
        // Normal path: the order is still in the open-orders list.
        {
            std::unique_lock<std::shared_mutex> write_lock(m_mutex);
            completed_report = applyUpdateAndMaybeComplete(order_id, found);
            callback_copy = m_completionCallback;
        }
    }
    else
    {
        // The order left the list — either cancelled (deleted) or filled
        // (a filled market order disappears immediately). Snapshot the
        // tracked order's metadata, then check positions for a fresh fill.
        std::optional<TrackedOrder> tracked;
        {
            std::shared_lock<std::shared_mutex> read_lock(m_mutex);
            auto it = m_trackedOrders.find(order_id);
            if (it != m_trackedOrders.end())
            {
                tracked = it->second;
            }
        }
        if (!tracked)
        {
            return OrderStatus::Pending; // Not tracked — nothing to update.
        }

        // Key-match fallback: the list may carry the EXCHANGE order id while
        // we track the POST's data.id (the list read lagged the POST, or the
        // id-resolution retries were exhausted — verified 2026-08-17: a
        // trigger order tracked as data.id 47777 was never found by id and
        // mis-cancelled while the real 17654490 was still open). Match by
        // symbol/side/volume/price/time instead, then re-anchor the tracking
        // key to the real id before applying the update.
        const std::int64_t submit_sec =
            std::chrono::duration_cast<std::chrono::seconds>(
                tracked->submit_time.time_since_epoch())
                .count();
        found = findCfdOrderByKey(list, tracked->symbol, tracked->side,
                                  tracked->requested_qty, tracked->type,
                                  tracked->submit_mid_price, submit_sec);
        if (!found.is_null())
        {
            const std::string real_id = orderIdToString(found["order_id"]);
            if (real_id != order_id)
            {
                PULSE_LOG_WARN("execution",
                    "CFD order {} re-anchored to exchange id {}", order_id,
                    real_id);
                // Re-anchor under the write lock, then treat the poll as if
                // it had been for the real id. Keep the data.id → real-id
                // alias so callers (cancel_order) can resolve the tracked key.
                std::unique_lock<std::shared_mutex> write_lock(m_mutex);
                m_idAliases[order_id] = real_id;
                auto node = m_trackedOrders.extract(order_id);
                if (!node.empty())
                {
                    node.key() = real_id;
                    m_trackedOrders.insert(std::move(node));
                    completed_report =
                        applyUpdateAndMaybeComplete(real_id, found);
                    callback_copy = m_completionCallback;
                }
            }
            else
            {
                std::unique_lock<std::shared_mutex> write_lock(m_mutex);
                completed_report = applyUpdateAndMaybeComplete(order_id, found);
                callback_copy = m_completionCallback;
            }
            if (completed_report)
            {
                if (callback_copy)
                {
                    callback_copy(*completed_report);
                }
                return completed_report->final_status;
            }
            return getStatus(order_id).value_or(OrderStatus::Pending);
        }

        PULSE_LOG_WARN("execution",
            "CFD order {} not in the open-orders list — checking positions "
            "for a fill before declaring it cancelled",
            order_id);

        bool matched_fill = false;
        auto pos_result = m_restClient.getCfdPositions();
        if (ok(pos_result))
        {
            const auto &pos_data =
                value(pos_result).value("data", nlohmann::json::object());
            const auto &pos_list = pos_data.value("list", nlohmann::json::array());
            if (pos_list.is_array())
            {
                const std::int64_t submit_sec =
                    std::chrono::duration_cast<std::chrono::seconds>(
                        tracked->submit_time.time_since_epoch())
                        .count();
                for (const auto &p : pos_list)
                {
                    if (p.value("symbol", "") != tracked->symbol)
                    {
                        continue;
                    }
                    const bool is_long = ("Long" == p.value("position_dir", ""));
                    if (is_long != (Side::Buy == tracked->side))
                    {
                        continue;
                    }
                    if (std::abs(jsonNumOrStr(p, "volume", 0.0)
                                 - tracked->requested_qty) > 1e-9)
                    {
                        continue;
                    }
                    // Filled within [submit-5s, submit+120s] — MT5 fill
                    // registration can lag the POST by tens of seconds.
                    const std::int64_t opened = p.value("time_create", 0LL);
                    if (opened < submit_sec - 5 || opened > submit_sec + 120)
                    {
                        continue;
                    }

                    // Synthesize a filled update object from the position.
                    nlohmann::json fill;
                    fill["order_id"] = order_id;
                    fill["state"] = 2; // Terminal (filled).
                    fill["finished"] = 1;
                    fill["filled_volume"] = p["volume"];
                    fill["fill_price"] = p["price_open"];
                    fill["fee"] = p["commission"];
                    // The close endpoint needs the EXCHANGE position id (the
                    // internal position_id is engine-local).
                    fill["exchange_position_id"] = p["position_id"];
                    std::unique_lock<std::shared_mutex> write_lock(m_mutex);
                    completed_report = applyUpdateAndMaybeComplete(order_id, fill);
                    callback_copy = m_completionCallback;
                    matched_fill = true;
                    break;
                }
            }
        }
        if (!matched_fill)
        {
            // No fresh position — treat as cancelled. filled_qty stays 0 so
            // onOrderComplete cannot open a phantom position.
            nlohmann::json cancelled;
            cancelled["order_id"] = order_id;
            cancelled["state"] = 1; // Deleted.
            cancelled["finished"] = 1;
            std::unique_lock<std::shared_mutex> write_lock(m_mutex);
            completed_report = applyUpdateAndMaybeComplete(order_id, cancelled);
            callback_copy = m_completionCallback;
        }
    }

    // Invoke callback outside lock — no lock-ordering coupling.
    if (completed_report)
    {
        if (callback_copy)
        {
            callback_copy(*completed_report);
        }
        return completed_report->final_status;
    }
    return getStatus(order_id).value_or(OrderStatus::Pending);
}

std::optional<ExecutionReport> OrderTracker::applyUpdateAndMaybeComplete(
    const std::string &order_id, const nlohmann::json &update)
{
    // Caller holds the write lock (m_mutex) — this is the single lock-held
    // state machine shared by the WS path, the REST poll path and the CFD
    // list-poll path.
    auto it = m_trackedOrders.find(order_id);
    if (it == m_trackedOrders.end())
    {
        return std::nullopt; // Not tracking this order.
    }

    applyOrderUpdate(update, it->second);

    // Terminal state — generate the report, stash it, erase the order.
    if (isTerminalStatus(it->second.status))
    {
        auto report = generateReport(it->second, now());
        m_completedReports[order_id] = report;

        PULSE_LOG_INFO("execution",
            "Order completed: {} status={} filled_qty={} avg_price={} slippage={}bps",
            order_id, update.value("status", ""), report.filled_qty,
            report.avg_fill_price, report.slippage_bps);

        m_trackedOrders.erase(it);
        return report;
    }
    return std::nullopt;
}

void OrderTracker::reconcileAll()
{
    // Snapshot tracked ids under a shared lock, then poll each — pollOrderStatus
    // takes the write lock itself and may erase terminal orders mid-iteration.
    std::vector<std::string> order_ids;
    {
        std::shared_lock<std::shared_mutex> read_lock(m_mutex);
        order_ids.reserve(m_trackedOrders.size());
        for (const auto &[id, order] : m_trackedOrders)
        {
            order_ids.push_back(id);
        }
    }

    for (const auto &order_id : order_ids)
    {
        auto result = pollOrderStatus(order_id);
        if (!ok(result))
        {
            PULSE_LOG_DEBUG("execution", "reconcile order {} failed: {}",
                order_id, error(result).message);
        }
    }
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

    // Futures ids arrive as integers — normalize before lookup.
    const std::string order_id = orderIdToString(update["id"]);

    PULSE_LOG_DEBUG("execution", "Order update: {} status={} finish_as={}",
        order_id, update.value("status", ""), update.value("finish_as", ""));

    // Prepare callback data under lock, invoke outside lock to avoid
    // lock-ordering coupling with downstream mutexes (PositionManager, etc.)
    std::optional<ExecutionReport> completed_report;
    CompletionCallback callback_copy;

    {
        std::unique_lock<std::shared_mutex> write_lock(m_mutex);
        completed_report = applyUpdateAndMaybeComplete(order_id, update);
        callback_copy = m_completionCallback;
    } // write_lock released

    // Invoke callback outside lock — no lock-ordering coupling
    if (completed_report && callback_copy)
    {
        callback_copy(*completed_report);
    }
}

void OrderTracker::applyOrderUpdate(const nlohmann::json &update, TrackedOrder &order)
{
    // TradFi CFD order objects use state/finished (ints) instead of the
    // status string; fills carry filled_volume/fill_price/fee (the fill
    // fields are synthesized by pollCfdOrderStatus from the position object).
    if (MarketType::Cfd == m_marketType)
    {
        order.status = parseCfdOrderStatus(update);
        if (update.contains("filled_volume"))
        {
            order.filled_qty = jsonNumOrStr(update, "filled_volume", order.filled_qty);
        }
        else if (OrderStatus::Filled == order.status)
        {
            order.filled_qty = order.requested_qty;
        }
        if (update.contains("fill_price"))
        {
            order.avg_fill_price = jsonNumOrStr(update, "fill_price", order.avg_fill_price);
        }
        if (update.contains("fee"))
        {
            order.fees = jsonNumOrStr(update, "fee", order.fees);
        }
        if (update.contains("exchange_position_id"))
        {
            order.exchange_position_id =
                orderIdToString(update["exchange_position_id"]);
        }
        order.last_update_time = now();
        return;
    }

    const std::string status_str = update.value("status", "");
    const std::string finish_as  = update.value("finish_as", "");
    order.status = parseFuturesStatus(status_str, finish_as);
    order.last_update_time = now();

    // Futures: signed contract counts (size/left) + fill price.
    // size=1 left=0 -> fully filled 1 contract; size=-1 left=1 -> 0 filled.
    if (update.contains("size") && update.contains("left"))
    {
        const double filled = std::abs(jsonNumOrStr(update, "size", 0.0))
                            - std::abs(jsonNumOrStr(update, "left", 0.0));
        order.filled_qty = (filled > 0.0) ? filled : 0.0;

        const double fill_price = jsonNumOrStr(update, "fill_price", 0.0);
        if (fill_price > 0.0)
        {
            order.avg_fill_price = fill_price;
        }
    }

    // Spot: string-typed fill fields (absent on futures responses).
    if (update.contains("filled_total"))
    {
        order.filled_qty = jsonNumOrStr(update, "filled_total", order.filled_qty);
    }
    if (update.contains("avg_deal_price"))
    {
        order.avg_fill_price = jsonNumOrStr(update, "avg_deal_price", order.avg_fill_price);
    }

    order.fees = jsonNumOrStr(update, "fee", order.fees);
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
    report.exchange_position_id = order.exchange_position_id;

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
    if ("closed" == status_str || "finished" == status_str)
    {
        return OrderStatus::Filled;
    }
    if ("cancelled" == status_str)
    {
        return OrderStatus::Cancelled;
    }
    return OrderStatus::Pending;
}

OrderStatus OrderTracker::parseFuturesStatus(const std::string &status,
                                             const std::string &finish_as)
{
    if ("filled" == finish_as)
    {
        return OrderStatus::Filled;
    }
    if ("cancelled" == finish_as || "reduce_only" == finish_as
        || "position_closed" == finish_as)
    {
        return OrderStatus::Cancelled;
    }
    if ("open" == finish_as || "open" == status)
    {
        return OrderStatus::Open;
    }
    if ("finished" == status)
    {
        // Terminal but finish_as missing (defensive — Gate always sends it).
        return OrderStatus::Filled;
    }
    return parseStatus(status);
}

OrderStatus OrderTracker::parseCfdOrderStatus(const nlohmann::json &order_obj)
{
    // TradFi CFD order objects use ints: state + finished (probe-verified
    // 2026-08-17: open orders report state=1, finished=0). Terminal mapping:
    // state==1 with finished!=0 = deleted (cancelled); any other terminal
    // state = filled. A filled_qty field is authoritative when present.
    if (!order_obj.is_object())
    {
        return OrderStatus::Pending;
    }
    const int state = order_obj.value("state", 0);
    const int finished = order_obj.value("finished", 0);

    if (0 != finished)
    {
        return (1 == state) ? OrderStatus::Cancelled : OrderStatus::Filled;
    }
    return (state >= 1) ? OrderStatus::Open : OrderStatus::Pending;
}

nlohmann::json OrderTracker::findCfdOrderInList(const nlohmann::json &list,
                                                const std::string &order_id)
{
    if (!list.is_array())
    {
        return nullptr;
    }
    for (const auto &o : list)
    {
        if (!o.is_object() || !o.contains("order_id"))
        {
            continue;
        }
        // The list encodes order_id as int (probe-verified) — normalize.
        if (orderIdToString(o["order_id"]) == order_id)
        {
            return o;
        }
    }
    return nullptr;
}

nlohmann::json OrderTracker::findCfdOrderByKey(
    const nlohmann::json &list, const Symbol &symbol, Side side,
    Quantity qty, OrderType type, Price price, std::int64_t submit_sec)
{
    if (!list.is_array())
    {
        return nullptr;
    }
    const int want_side = (Side::Buy == side) ? 2 : 1;
    constexpr std::int64_t kCfdMatchSlackSec = 5; // Covers clock skew.

    for (const auto &o : list)
    {
        if (!o.is_object())
        {
            continue;
        }
        if (o.value("symbol", "") != symbol || o.value("side", 0) != want_side)
        {
            continue;
        }
        const double volume = jsonNumOrStr(o, "volume", -1.0);
        if (volume < 0.0 || std::abs(volume - qty) > 1e-9)
        {
            continue;
        }
        // Only orders placed AFTER our POST can be ours (time_setup=0/missing
        // is outside the window). Market orders match on the key fields only;
        // trigger orders must also agree on price.
        const std::int64_t time_setup = o.value("time_setup", 0LL);
        if (time_setup < submit_sec - kCfdMatchSlackSec)
        {
            continue;
        }
        if (OrderType::Market != type)
        {
            const double list_price = jsonNumOrStr(o, "price", -1.0);
            if (list_price < 0.0 || std::abs(list_price - price) > 1e-6)
            {
                continue;
            }
        }
        return o;
    }
    return nullptr;
}

void OrderTracker::testSimulateCfdPoll(const nlohmann::json &order_obj)
{
    if (!order_obj.is_object() || !order_obj.contains("order_id"))
    {
        return;
    }
    const std::string order_id = orderIdToString(order_obj["order_id"]);

    // Same lock-held state machine as pollCfdOrderStatus — network-free.
    std::optional<ExecutionReport> completed_report;
    CompletionCallback callback_copy;
    {
        std::unique_lock<std::shared_mutex> write_lock(m_mutex);
        completed_report = applyUpdateAndMaybeComplete(order_id, order_obj);
        callback_copy = m_completionCallback;
    }
    if (completed_report && callback_copy)
    {
        callback_copy(*completed_report);
    }
}

std::string OrderTracker::resolveExchangeId(const std::string &order_id) const
{
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    auto it = m_idAliases.find(order_id);
    return (m_idAliases.end() == it) ? order_id : it->second;
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
