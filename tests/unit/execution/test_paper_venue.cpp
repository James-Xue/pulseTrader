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

TEST_F(PaperVenueTest, JsonRoundTripRestoresThePositionBook)
{
    placeAndFill(marketOrder(Side::Buy, 0.01, 3995.0, 4005.0));
    ASSERT_EQ(1u, venue.openPositionCount());
    const nlohmann::json saved = venue.toJson();

    PaperConfig restored_cfg = cfg;
    PaperAccount restored_account{ restored_cfg };
    PaperVenue restored{ restored_cfg, { "XAUUSD" }, restored_account };

    ASSERT_TRUE(restored.restore(saved));
    EXPECT_EQ(1u, restored.openPositionCount());
    const auto rows = restored.openPositions(MarketType::Cfd);
    ASSERT_EQ(1u, rows.size());
    EXPECT_DOUBLE_EQ(4000.0, rows[0].entry_price);
    EXPECT_DOUBLE_EQ(3995.0, rows[0].sl_price);
    EXPECT_DOUBLE_EQ(4005.0, rows[0].tp_price);
    EXPECT_EQ("test-cid", rows[0].client_order_id);
    EXPECT_FALSE(rows[0].exchange_position_id.empty());
}

TEST_F(PaperVenueTest, RestoreRejectsMalformedPayload)
{
    EXPECT_FALSE(venue.restore(nlohmann::json::object()));
    EXPECT_FALSE(venue.restore(
        nlohmann::json{ { "positions", nlohmann::json::array(
                                             { nlohmann::json::object() }) } }));
}

// ---------------------------------------------------------------------------
// P1 of the 2026-11-27 paper-team reconciliation audit: the exit event is the
// ledger's only venue-truth source, so it must carry the economics, not just
// the fill price. These tests are red on the pre-P1 struct (it has no
// realized_pnl / entry_price / client_order_id / market_type / leverage /
// quanto fields).
// ---------------------------------------------------------------------------

TEST_F(PaperVenueTest, ExitEventCarriesRealizedPnlAndEntryPrice)
{
    // Short 0.01 lot at 3621.38. The protective stop sits at 3625.00 and the
    // mark gaps to 3625.15, so the venue exits at the observed mark (a gap is
    // not forgiven).
    price = 3621.38;
    OrderRequest req = marketOrder(Side::Sell, 0.01, 3625.00, 0.0);
    req.leverage = 1000.0;
    req.client_order_id = "xau_team:xau_bouncy:000001";
    placeAndFill(req);
    ASSERT_EQ(1u, venue.openPositionCount());

    price = 3625.15;
    venue.tick(stamp + 1);

    const auto exits = venue.drainExitEvents();
    ASSERT_EQ(1u, exits.size());
    const PaperExitEvent &exit = exits[0];

    // -3.77 = (3621.38 - 3625.15) * 0.01 lot * 100 oz/lot. The ledger must use
    // the venue's number, not recompute it from PositionManager.
    EXPECT_NEAR(-3.77, exit.realized_pnl, 1e-9);
    EXPECT_DOUBLE_EQ(3621.38, exit.entry_price);
    EXPECT_NEAR(0.06, exit.fee, 1e-9); // closing a short is the BUY leg
    EXPECT_EQ("xau_team:xau_bouncy:000001", exit.client_order_id);
    EXPECT_EQ(MarketType::Cfd, exit.market_type);
    EXPECT_DOUBLE_EQ(1000.0, exit.leverage);
    EXPECT_DOUBLE_EQ(100.0, exit.quanto);
}

TEST_F(PaperVenueTest, ShortCloseChargesBuyLegCommission)
{
    // Gate TradFi bills the commission on the BUY leg only: opening a short is
    // free, closing it costs 6 USDT per lot.
    price = 3600.0;
    placeAndFill(marketOrder(Side::Sell, 0.01));
    ASSERT_EQ(1u, venue.openPositionCount());

    const auto rows = venue.openPositions(MarketType::Cfd);
    ASSERT_EQ(1u, rows.size());
    const auto closed = venue.closeCfdPosition(rows[0].exchange_position_id,
                                               0.01, 0.01);
    ASSERT_TRUE(ok(closed));
    EXPECT_NEAR(0.06, value(closed).fee, 1e-9);
    EXPECT_NEAR(0.06, account.snapshot().cfd.fees_paid_usdt, 1e-9);
}

TEST_F(PaperVenueTest, LongCloseChargesNoCommission)
{
    // The mirror case: entry pays 0.06, the SELL close leg is free.
    price = 3600.0;
    placeAndFill(marketOrder(Side::Buy, 0.01));
    ASSERT_EQ(1u, venue.openPositionCount());

    const auto rows = venue.openPositions(MarketType::Cfd);
    ASSERT_EQ(1u, rows.size());
    const auto closed = venue.closeCfdPosition(rows[0].exchange_position_id,
                                               0.01, 0.01);
    ASSERT_TRUE(ok(closed));
    EXPECT_NEAR(0.0, value(closed).fee, 1e-12);
    EXPECT_NEAR(0.06, account.snapshot().cfd.fees_paid_usdt, 1e-9);
}

TEST_F(PaperVenueTest, TwoExitsSamePositionPartialClose)
{
    // Two 0.005 closes must each charge a pro-rata commission and release a
    // pro-rata margin, and the second must retire the position cleanly.
    price = 3600.0;
    placeAndFill(marketOrder(Side::Sell, 0.01));
    const auto rows = venue.openPositions(MarketType::Cfd);
    ASSERT_EQ(1u, rows.size());
    const std::string pid = rows[0].exchange_position_id;
    const double full_margin = rows[0].locked_margin;

    const auto first = venue.closeCfdPosition(pid, 0.005, 0.01);
    ASSERT_TRUE(ok(first));
    EXPECT_NEAR(0.03, value(first).fee, 1e-9);
    EXPECT_NEAR(full_margin / 2.0, value(first).margin_released, 1e-9);
    {
        const auto partial = venue.openPositions(MarketType::Cfd);
        ASSERT_EQ(1u, partial.size());
        EXPECT_NEAR(0.005, partial[0].lots, 1e-12);
        EXPECT_NEAR(full_margin / 2.0, partial[0].locked_margin, 1e-9);
    }

    const auto second = venue.closeCfdPosition(pid, 0.005, 0.005);
    ASSERT_TRUE(ok(second));
    EXPECT_NEAR(0.03, value(second).fee, 1e-9);
    EXPECT_EQ(0u, venue.openPositionCount());
    EXPECT_NEAR(0.06, account.snapshot().cfd.fees_paid_usdt, 1e-9);
}

} // namespace
