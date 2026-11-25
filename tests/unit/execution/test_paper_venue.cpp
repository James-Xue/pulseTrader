// test_paper_venue.cpp — In-process simulated exchange (P2: CFD).
//
// Pins the two load-bearing invariants:
//   1. place() never fills and never fires the completion callback.
//   2. A venue exit is exposed as data; the engine (L9) owns the close.

#include "execution/PaperVenue.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <string>
#include <vector>

using namespace pulse;
using namespace pulse::execution;

namespace
{

std::int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

class PaperVenueTest : public ::testing::Test
{
  protected:
    /// Constructed before the venue, so the contract table must be complete
    /// here (members are built before SetUp runs).
    static PaperConfig makeConfig()
    {
        PaperConfig config;
        config.contract_volume["XAUUSD"] = 100.0;
        return config;
    }

    PaperConfig cfg{ makeConfig() };
    PaperAccount account{ cfg };
    PaperVenue venue{ cfg, { "XAUUSD" }, account };
    double price{ 4000.0 };
    std::int64_t stamp{ nowMs() };
    std::vector<ExecutionReport> reports;

    void SetUp() override
    {
        venue.setMarkProvider(
            [this](const Symbol &, MarketType)
                -> std::optional<PaperMark>
            { return PaperMark{ price, stamp, price, price }; });
        venue.setCompletionCallback(
            [this](const ExecutionReport &report)
            { reports.push_back(report); });
    }

    OrderRequest marketOrder(Side side, double qty = 0.01,
                             double sl = 0.0, double tp = 0.0)
    {
        OrderRequest req;
        req.symbol = "XAUUSD";
        req.side = side;
        req.type = OrderType::Market;
        req.quantity = qty;
        req.market_type = MarketType::Cfd;
        req.leverage = 1.0;
        req.quanto_multiplier = 100.0;
        req.client_order_id = "test-cid";
        if (sl > 0.0)
        {
            req.sl_price = sl;
        }
        if (tp > 0.0)
        {
            req.tp_price = tp;
        }
        return req;
    }

    std::string placeAndFill(const OrderRequest &req)
    {
        const auto placed = venue.place(req);
        EXPECT_TRUE(ok(placed)) << error(placed).message;
        const std::string id = value(placed).order_id;
        venue.trackOrder(id, req.symbol, req.side, req.type, req.quantity,
                         price, req.client_order_id);
        const auto polled = venue.pollOrderStatus(id);
        EXPECT_TRUE(ok(polled));
        return id;
    }
};

TEST_F(PaperVenueTest, PlaceDoesNotFillOrFireTheCallback)
{
    const auto placed = venue.place(marketOrder(Side::Buy));
    ASSERT_TRUE(ok(placed));
    EXPECT_EQ(OrderStatus::Open, value(placed).status);
    EXPECT_TRUE(reports.empty());
    EXPECT_EQ(0u, venue.openPositionCount());
}

TEST_F(PaperVenueTest, PollFillsExactlyOnceAndOpensAVenuePosition)
{
    const auto req = marketOrder(Side::Buy, 0.01, 3995.0, 4005.0);
    const std::string id = placeAndFill(req);

    ASSERT_EQ(1u, reports.size());
    EXPECT_EQ(OrderStatus::Filled, reports[0].final_status);
    EXPECT_EQ(id, reports[0].order_id);
    EXPECT_DOUBLE_EQ(4000.0, reports[0].avg_fill_price);
    EXPECT_FALSE(reports[0].exchange_position_id.empty());
    EXPECT_EQ(1u, venue.openPositionCount());

    // A second poll returns the same terminal status without re-firing.
    ASSERT_TRUE(ok(venue.pollOrderStatus(id)));
    EXPECT_EQ(OrderStatus::Filled, value(venue.pollOrderStatus(id)));
    EXPECT_EQ(1u, reports.size());
    EXPECT_EQ(1u, venue.openPositionCount());
}

TEST_F(PaperVenueTest, FillLocksMarginAndChargesTheBuyLegCommission)
{
    placeAndFill(marketOrder(Side::Buy));

    const auto snapshot = venue.accountSnapshot();
    // 0.01 lot x 100 oz x 4000 = 4000 notional; leverage 1 -> 4000 margin.
    EXPECT_DOUBLE_EQ(4000.0, snapshot.cfd.locked_margin_usdt);
    // 6 USDT/lot buy-side commission = 0.06.
    EXPECT_DOUBLE_EQ(0.06, snapshot.cfd.fees_paid_usdt);
    EXPECT_DOUBLE_EQ(9999.94 - 4000.0, snapshot.cfd.cash_usdt);
    EXPECT_NEAR(29999.94, snapshot.equity_usdt, 1e-6);
}

TEST_F(PaperVenueTest, ShortOpenPaysNoCommission)
{
    placeAndFill(marketOrder(Side::Sell));
    EXPECT_DOUBLE_EQ(0.0, venue.accountSnapshot().cfd.fees_paid_usdt);
}

TEST_F(PaperVenueTest, StaleMarkRefusesAMarketOrder)
{
    stamp -= static_cast<std::int64_t>(cfg.max_mark_age_ms) + 1000;
    const auto placed = venue.place(marketOrder(Side::Buy));
    EXPECT_FALSE(ok(placed));
    EXPECT_EQ(0u, venue.openPositionCount());
}

TEST_F(PaperVenueTest, CancelEmitsOneCancelledReport)
{
    auto req = marketOrder(Side::Buy);
    req.type = OrderType::Limit;
    req.price = 3900.0; // below the mark: rests, does not cross
    const auto placed = venue.place(req);
    ASSERT_TRUE(ok(placed));
    const std::string id = value(placed).order_id;
    venue.trackOrder(id, req.symbol, req.side, req.type, req.quantity, price,
                     req.client_order_id);

    EXPECT_TRUE(venue.cancel(id));
    ASSERT_EQ(1u, reports.size());
    EXPECT_EQ(OrderStatus::Cancelled, reports[0].final_status);

    // A second cancel finds a terminal order and reports nothing.
    EXPECT_FALSE(venue.cancel(id));
    EXPECT_EQ(1u, reports.size());
}

TEST_F(PaperVenueTest, EngineCloseReleasesMarginAndEmitsNoExitEvent)
{
    placeAndFill(marketOrder(Side::Buy));
    ASSERT_EQ(1u, reports.size());
    const std::string venue_id = reports[0].exchange_position_id;
    price = 4010.0;

    const auto closed = venue.closeCfdPosition(venue_id, 0.01, 0.01);
    ASSERT_TRUE(ok(closed));
    EXPECT_DOUBLE_EQ(4010.0, value(closed).exit_price);
    EXPECT_DOUBLE_EQ(10.0, value(closed).realized_pnl);
    EXPECT_DOUBLE_EQ(4000.0, value(closed).margin_released);
    EXPECT_EQ(0u, venue.openPositionCount());
    // Engine-initiated close: L9 records it via recordCfdClose.
    EXPECT_TRUE(venue.drainExitEvents().empty());
}

TEST_F(PaperVenueTest, StopLossExitsAtTheObservedMarkAndQueuesAnEvent)
{
    placeAndFill(marketOrder(Side::Buy, 0.01, 3995.0, 4005.0));

    // Gap through the stop: the fill must be the mark, not the stop level.
    price = 3990.0;
    venue.tick(nowMs());

    EXPECT_EQ(0u, venue.openPositionCount());
    const auto events = venue.drainExitEvents();
    ASSERT_EQ(1u, events.size());
    EXPECT_EQ(PaperExitReason::StopLoss, events[0].reason);
    EXPECT_DOUBLE_EQ(3990.0, events[0].fill_price);
    EXPECT_DOUBLE_EQ(-10.0, venue.accountSnapshot().cfd.realized_pnl_usdt);
}

TEST_F(PaperVenueTest, TakeProfitFillsAtItsOwnPrice)
{
    placeAndFill(marketOrder(Side::Buy, 0.01, 3995.0, 4005.0));

    price = 4012.0;
    venue.tick(nowMs());

    const auto events = venue.drainExitEvents();
    ASSERT_EQ(1u, events.size());
    EXPECT_EQ(PaperExitReason::TakeProfit, events[0].reason);
    EXPECT_DOUBLE_EQ(4005.0, events[0].fill_price);
    EXPECT_DOUBLE_EQ(5.0, venue.accountSnapshot().cfd.realized_pnl_usdt);
}

TEST_F(PaperVenueTest, ModifyStopsUpdatesTheVenueBook)
{
    placeAndFill(marketOrder(Side::Buy, 0.01, 3995.0, 4005.0));
    ASSERT_EQ(1u, reports.size());
    const std::string venue_id = reports[0].exchange_position_id;

    const auto modified = venue.modifyCfdStops(venue_id, 3998.0, 4010.0);
    ASSERT_TRUE(ok(modified));

    const auto stops = venue.cfdStopState(venue_id);
    ASSERT_TRUE(stops.has_value());
    EXPECT_DOUBLE_EQ(3998.0, stops->sl_price);
    EXPECT_DOUBLE_EQ(4010.0, stops->tp_price);

    const auto rows = venue.openPositions(MarketType::Cfd);
    ASSERT_EQ(1u, rows.size());
    EXPECT_DOUBLE_EQ(3998.0, rows[0].sl_price);
}

TEST_F(PaperVenueTest, MissingContractVolumeIsRejected)
{
    OrderRequest req = marketOrder(Side::Buy);
    req.symbol = "XAU_USDT"; // no [paper.contract_volume] entry
    const auto placed = venue.place(req);
    EXPECT_FALSE(ok(placed));
}

} // namespace
