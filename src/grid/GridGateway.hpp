#pragma once
// grid_gateway.hpp — production IGridGateway (M27)
//
// Forwards the grid's exchange-facing operations to the engine's real
// components: OrderFlowExecutor::placeManualOrder (FULL risk gate —
// evaluate → reserve → execute → track) for placement, the shared cancel
// path, the futures REST client for the exchange-order truth view, and the
// PositionManager for the user-share warning.
//
// Deliberately NOT part of the pulse_grid library (it would create a
// pulse_grid → pulse_control link cycle); it is compiled into the engine
// executable instead.

#include "control/OrderFlowExecutor.hpp"
#include "exchange/GateRestClient.hpp"
#include "grid/IGridGateway.hpp"
#include "risk/PositionManager.hpp"

namespace pulse::grid
{

class GridGateway : public IGridGateway
{
  public:
    GridGateway(pulse::control::OrderFlowExecutor &order_flow,
                exchange::GateRestClient *futures_rest,
                risk::PositionManager &position_mgr,
                std::mutex &rest_mutex)
        : m_orderFlow{ order_flow }
        , m_futuresRest{ futures_rest }
        , m_positionMgr{ position_mgr }
        , m_restMutex{ rest_mutex }
    {
    }

    Result<execution::OrderResponse> place(
        const execution::OrderRequest &req) override
    {
        // Manual path: M22 relaxed direction gate — futures orders execute
        // in any active direction, bounded by the futures notional budget.
        return m_orderFlow.placeManualOrder(req);
    }

    bool cancel(const std::string &order_id) override
    {
        // Direct exchange cancel first: tracker-based cancelOrder cannot see
        // orders that predate a restart, and the grid must be able to tear
        // down leftover eth-grid-* orders after an engine restart.
        if (nullptr != m_futuresRest)
        {
            std::lock_guard lock{ m_restMutex };
            auto direct = m_futuresRest->cancelFuturesOrder(order_id);
            if (ok(direct))
            {
                return true;
            }
        }
        return m_orderFlow.cancelOrder(order_id);
    }

    std::vector<ExchangeOrderView> openFuturesOrders(
        const std::string &contract) override
    {
        std::vector<ExchangeOrderView> views;
        if (nullptr == m_futuresRest)
        {
            return views;
        }
        std::lock_guard lock{ m_restMutex };
        auto result = m_futuresRest->getFuturesOrders(contract);
        if (!ok(result))
        {
            return views;
        }
        for (const auto &o : value(result))
        {
            ExchangeOrderView v;
            v.order_id = o.value("id", "");
            v.client_order_id = o.value("text", "");
            v.price = o.value("price", 0.0);
            v.quantity = o.value("size", 0.0);
            v.reduce_only = o.value("reduce_only", false);
            v.status = o.value("status", "");
            views.push_back(std::move(v));
        }
        return views;
    }

    std::vector<PositionView> positionsBySymbol(
        const std::string &symbol) override
    {
        std::vector<PositionView> views;
        for (const auto &pos : m_positionMgr.getAllPositions())
        {
            if (pos.symbol != symbol || pos.quantity <= 0.0)
            {
                continue;
            }
            PositionView v;
            v.symbol = pos.symbol;
            v.strategy_id = pos.strategy_id;
            v.quantity = pos.side == Side::Sell ? -pos.quantity : pos.quantity;
            v.entry_price = pos.entry_price;
            v.notional_value = pos.notional_value;
            v.is_short = pos.side == Side::Sell;
            views.push_back(std::move(v));
        }
        return views;
    }

  private:
    pulse::control::OrderFlowExecutor &m_orderFlow;
    exchange::GateRestClient *m_futuresRest; ///< Nullable (no futures wiring).
    risk::PositionManager &m_positionMgr;
    std::mutex &m_restMutex;
};

} // namespace pulse::grid
