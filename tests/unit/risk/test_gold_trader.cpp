// test_gold_trader.cpp — Unit tests for the USD dynamic stop supervisor (L7)
//
// The policy under test:
//   1. hard stop at -8 USD unrealized PnL
//   2. break-even once peak profit exceeds +2 USD
//   3. 1m SuperTrend candidate with a 1 USD buffer (and an immediate exit on
//      a trend flip against the position)
//   4. peak profit - 5 USD give-back, closed at market when reached
//   5. stops only tighten, never loosen
//
// Every test drives the supervisor with a fake ticker and a fake SuperTrend,
// so no network or market feed is involved.

#include "risk/GoldTrader.hpp"

#include "risk/PositionManager.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <optional>
#include <vector>

using namespace pulse;
using namespace pulse::risk;

namespace
{

RiskConfig make_risk_config()
{
    RiskConfig cfg;
    cfg.maxPositionNotional = 1e9;
    cfg.maxPositionNotionalCfd = 1e9;
    cfg.maxSymbolNotional = 1e9;
    cfg.maxOpenPositions = 20;
    return cfg;
}

GoldTraderConfig make_gold_config()
{
    GoldTraderConfig cfg;
    cfg.enabled = true;
    cfg.symbol = "XAUUSD";
    cfg.market_type = MarketType::Cfd;
    cfg.hard_stop_usd = 8.0;
    cfg.breakeven_trigger_usd = 2.0;
    cfg.supertrend_buffer_usd = 1.0;
    cfg.peak_drawdown_usd = 5.0;
    cfg.supertrend_period = 10.0;
    cfg.supertrend_multiplier = 3.0;
    cfg.poll_interval_ms = 1000;
    cfg.retry_cooldown_sec = 10;
    cfg.exchange_stop = true;
    return cfg;
}

Timestamp at_seconds(std::int64_t seconds)
{
    return Timestamp{ std::chrono::seconds{ seconds } };
}

// ---------------------------------------------------------------------------
// Harness — real PositionManager, fake market data, recording callbacks
// ---------------------------------------------------------------------------
struct Harness
{
    RiskConfig risk{ make_risk_config() };
    PositionManager positions{ risk };
    GoldTraderConfig config{ make_gold_config() };

    double price{ 0.0 };                       ///< 0 disables the quote.
    std::optional<SuperTrendPoint> supertrend; ///< Empty = warmup.
    bool stop_push_ok{ true };                 ///< Toggle push failures.
    bool close_ok{ true };                     ///< Toggle close failures.

    int closes{ 0 };        ///< Close callbacks fired.
    int stop_pushes{ 0 };   ///< Successful pushes.
    int stop_attempts{ 0 }; ///< All push attempts.
    Price last_close_mark{ 0.0 };
    std::vector<Price> pushed_stops;

    std::unique_ptr<GoldTrader> trader;

    explicit Harness(GoldTraderConfig cfg = make_gold_config()) : config{ std::move(cfg) }
    {
        trader = std::make_unique<GoldTrader>(
            config,
            positions,
            [this](const Symbol &) -> std::optional<Price>
            {
                if (price <= 0.0)
                {
                    return std::nullopt;
                }
                return price;
            },
            [this](const Symbol &) -> std::optional<SuperTrendPoint> { return supertrend; },
            [this](const Position &pos, Price mark)
            {
                if (!close_ok)
                {
                    return false;
                }
                ++closes;
                last_close_mark = mark;
                const auto closed = positions.closePosition(pos.position_id, pos.quantity, mark);
                return closed.has_value();
            },
            [this](const Position &pos, Price sl_price)
            {
                ++stop_attempts;
                if (!stop_push_ok)
                {
                    return false;
                }
                ++stop_pushes;
                pushed_stops.push_back(sl_price);
                positions.updateExchangeStops(pos.position_id, sl_price, pos.tp_price);
                return true;
            });
    }

    std::string open(Side side = Side::Buy,
        double quantity = 0.01,
        double entry = 4000.0,
        double sl_price = 0.0,
        double quanto = 100.0)
    {
        auto opened = positions.openPosition("XAUUSD",
            side,
            quantity,
            entry,
            "manual",
            MarketType::Cfd,
            500.0,
            MarginMode::Cross,
            quanto,
            0.0,
            sl_price);
        EXPECT_TRUE(ok(opened)) << error(opened).message;
        return ok(opened) ? value(opened) : std::string{};
    }

    /// Import an exchange position and return the engine-local id. Synced
    /// positions reuse "<symbol>_<side>_sync", which is what the sync-id
    /// reuse test needs.
    std::string sync(double quantity, double entry, double mark, const std::string &exchange_id)
    {
        positions.syncPositionFromExchange("XAUUSD",
            Side::Buy,
            quantity,
            entry,
            mark,
            MarketType::Cfd,
            500.0,
            MarginMode::Cross,
            100.0,
            0.0,
            0.0,
            Timestamp{},
            0.0,
            0.0,
            exchange_id);

        for (const auto &pos : positions.getAllPositions())
        {
            if (pos.exchange_position_id == exchange_id)
            {
                return pos.position_id;
            }
        }
        return {};
    }

    GoldTrader::TickResult tick(std::int64_t seconds)
    {
        return trader->tick(at_seconds(seconds));
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Hard stop
// ---------------------------------------------------------------------------

TEST(GoldTrader, HardStopFiresAtEightUsdLoss)
{
    Harness h;
    h.open(Side::Buy, 0.01, 4000.0);

    // 0.01 lot XAUUSD = 1 ounce: a 8.10 price move = -8.10 USD PnL.
    h.price = 3991.9;
    const auto result = h.tick(1);

    EXPECT_EQ(1, result.closed);
    EXPECT_EQ(1, h.closes);
    EXPECT_DOUBLE_EQ(3991.9, h.last_close_mark);
    EXPECT_EQ(0, h.stop_pushes); // breach branch does not push a stop

    // The close callback removed the position; the state is pruned next pass.
    (void)h.tick(2);
    EXPECT_EQ(0u, h.trader->trackedPositions());
    EXPECT_EQ(0, h.positions.openPositionCount());
}

TEST(GoldTrader, NoCloseAboveHardStopAndStopIsPushed)
{
    Harness h;
    h.open(Side::Buy, 0.01, 4000.0);

    h.price = 3999.0; // -1 USD PnL, above the -8 USD hard stop
    const auto result = h.tick(1);

    EXPECT_EQ(0, result.closed);
    EXPECT_EQ(1, result.stopPushed);
    ASSERT_EQ(1u, h.pushed_stops.size());
    EXPECT_DOUBLE_EQ(3992.0, h.pushed_stops[0]); // 4000 - 8, price == USD for 1 oz

    // Same price on the next pass: no duplicate REST call.
    const auto second = h.tick(2);
    EXPECT_EQ(0, second.stopPushed);
    EXPECT_EQ(1u, h.pushed_stops.size());
}

// ---------------------------------------------------------------------------
// Break-even
// ---------------------------------------------------------------------------

TEST(GoldTrader, BreakEvenArmsAfterTwoUsdPeakProfit)
{
    Harness h;
    h.open(Side::Buy, 0.01, 4000.0);

    // +3 USD peak: break-even (4000) is tighter than hard (3992) and the
    // peak give-back (3998).
    h.price = 4003.0;
    const auto first = h.tick(1);
    EXPECT_EQ(0, first.closed);
    EXPECT_EQ(1, first.stopPushed);
    ASSERT_FALSE(h.pushed_stops.empty());
    EXPECT_DOUBLE_EQ(4000.0, h.pushed_stops.back());

    // Price pulls back through the entry: close at market.
    h.price = 3999.9;
    const auto second = h.tick(2);
    EXPECT_EQ(1, second.closed);
    EXPECT_EQ(1, h.closes);
}

TEST(GoldTrader, BreakEvenAndGiveBackNotArmedBelowTrigger)
{
    Harness h;
    const auto id = h.open(Side::Buy, 0.01, 4000.0);

    // +1.5 USD is below the +2 USD trigger: neither break-even nor the peak
    // give-back is armed, so the -8 USD hard stop remains the floor. Arming
    // the give-back on any positive tick would put the stop at -3.50 here and
    // make the explicit hard stop unreachable.
    h.price = 4001.5;
    const auto result = h.tick(1);
    EXPECT_EQ(0, result.closed);
    const auto stop = h.trader->effectiveStop(id);
    ASSERT_TRUE(stop.has_value());
    EXPECT_DOUBLE_EQ(3992.0, stop.value());
}

// ---------------------------------------------------------------------------
// Peak give-back
// ---------------------------------------------------------------------------

TEST(GoldTrader, PeakGiveBackClosesAtMarket)
{
    Harness h;
    const auto id = h.open(Side::Buy, 0.01, 4000.0);

    // +10 USD peak -> give-back stop at +5 USD (4005).
    h.price = 4010.0;
    const auto first = h.tick(1);
    EXPECT_EQ(0, first.closed);
    const auto stop = h.trader->effectiveStop(id);
    ASSERT_TRUE(stop.has_value());
    EXPECT_DOUBLE_EQ(4005.0, stop.value());

    // Trading through the give-back level closes immediately at market.
    h.price = 4004.9;
    const auto second = h.tick(2);
    EXPECT_EQ(1, second.closed);
    EXPECT_EQ(1, h.closes);
    EXPECT_DOUBLE_EQ(4004.9, h.last_close_mark);
}

TEST(GoldTrader, PeakGiveBackUsesPeakNotCurrentProfit)
{
    Harness h;
    const auto id = h.open(Side::Buy, 0.01, 4000.0);

    h.price = 4008.0; // peak +8 -> give-back 4003
    (void)h.tick(1);

    h.price = 4004.0; // +4 USD current; stop stays at 4003 (tighten-only)
    const auto second = h.tick(2);
    EXPECT_EQ(0, second.closed);

    h.price = 4002.9; // through 4003 -> close
    const auto third = h.tick(3);
    EXPECT_EQ(1, third.closed);
}

// ---------------------------------------------------------------------------
// SuperTrend
// ---------------------------------------------------------------------------

TEST(GoldTrader, SuperTrendCandidateUsesOneUsdBuffer)
{
    GoldTraderConfig cfg = make_gold_config();
    cfg.hard_stop_usd = 100.0; // push the hard stop far away
    cfg.breakeven_trigger_usd = 100.0;
    cfg.exchange_stop = false;

    Harness h{ cfg };
    const auto id = h.open(Side::Buy, 0.01, 4000.0);
    h.price = 3995.0;
    h.supertrend = SuperTrendPoint{ .value = 3990.0, .direction = 1 };

    // Long + bullish SuperTrend -> stop = 3990 - 1 = 3989.
    (void)h.tick(1);
    const auto stop = h.trader->effectiveStop(id);
    ASSERT_TRUE(stop.has_value());
    EXPECT_DOUBLE_EQ(3989.0, stop.value());
    EXPECT_EQ(0, h.closes);
}

TEST(GoldTrader, SuperTrendNeverLoosensAnExistingStop)
{
    GoldTraderConfig cfg = make_gold_config();
    cfg.hard_stop_usd = 100.0;
    cfg.breakeven_trigger_usd = 100.0;
    cfg.exchange_stop = false;
    Harness h{ cfg };
    const auto id = h.open(Side::Buy, 0.01, 4000.0);

    h.price = 3995.0;
    h.supertrend = SuperTrendPoint{ .value = 3990.0, .direction = 1 };
    (void)h.tick(1); // candidate 3989 becomes effective

    // Trend moves up but the line drops: candidate 3969 must not loosen the
    // stop back to 3969.
    h.supertrend = SuperTrendPoint{ .value = 3970.0, .direction = 1 };
    (void)h.tick(2);
    const auto stop = h.trader->effectiveStop(id);
    ASSERT_TRUE(stop.has_value());
    EXPECT_DOUBLE_EQ(3989.0, stop.value());

    // A tighter candidate (3994) does move it.
    h.supertrend = SuperTrendPoint{ .value = 3995.0, .direction = 1 };
    (void)h.tick(3);
    const auto tighter = h.trader->effectiveStop(id);
    ASSERT_TRUE(tighter.has_value());
    EXPECT_DOUBLE_EQ(3994.0, tighter.value());
}

TEST(GoldTrader, SuperTrendFlipAgainstPositionForcesClose)
{
    GoldTraderConfig cfg = make_gold_config();
    cfg.hard_stop_usd = 100.0;
    cfg.breakeven_trigger_usd = 100.0;
    cfg.exchange_stop = false;
    Harness h{ cfg };
    h.open(Side::Buy, 0.01, 4000.0);

    h.price = 4005.0;
    h.supertrend = SuperTrendPoint{ .value = 4010.0, .direction = -1 };
    const auto result = h.tick(1);

    EXPECT_EQ(1, result.closed);
    EXPECT_EQ(1, h.closes);
}

// ---------------------------------------------------------------------------
// Short side
// ---------------------------------------------------------------------------

TEST(GoldTrader, ShortHardStopIsMirrored)
{
    Harness h;
    const auto id = h.open(Side::Sell, 0.01, 4000.0);

    h.price = 4008.1; // -8.10 USD for a short
    const auto result = h.tick(1);
    EXPECT_EQ(1, result.closed);

    (void)h.tick(2);
    EXPECT_EQ(0, h.positions.openPositionCount());
}

TEST(GoldTrader, ShortBreakEvenAndGiveBackTightenDownward)
{
    Harness h;
    const auto id = h.open(Side::Sell, 0.01, 4000.0);

    // +6 USD peak on a short -> price 3994; give-back stop 3999.
    h.price = 3994.0;
    (void)h.tick(1);
    const auto stop = h.trader->effectiveStop(id);
    ASSERT_TRUE(stop.has_value());
    EXPECT_DOUBLE_EQ(3999.0, stop.value());

    // Price rises through 3999 -> close.
    h.price = 3999.1;
    const auto second = h.tick(2);
    EXPECT_EQ(1, second.closed);
}

// ---------------------------------------------------------------------------
// Existing exchange stop
// ---------------------------------------------------------------------------

TEST(GoldTrader, AdoptsTighterExchangeStopAndNeverLoosensIt)
{
    Harness h;
    // A manually tightened stop at 3995 (tighter than the -8 USD hard stop).
    const auto id = h.open(Side::Buy, 0.01, 4000.0, 3995.0);

    h.price = 3999.0;
    (void)h.tick(1);
    const auto stop = h.trader->effectiveStop(id);
    ASSERT_TRUE(stop.has_value());
    EXPECT_DOUBLE_EQ(3995.0, stop.value());

    // The supervisor must not push a looser stop over the user's 3995.
    EXPECT_TRUE(h.pushed_stops.empty());

    h.price = 3994.9;
    const auto second = h.tick(2);
    EXPECT_EQ(1, second.closed);
}

// ---------------------------------------------------------------------------
// Failure handling
// ---------------------------------------------------------------------------

TEST(GoldTrader, FailedStopPushRetriesAfterCooldown)
{
    Harness h;
    h.open(Side::Buy, 0.01, 4000.0);
    h.stop_push_ok = false;
    h.price = 3999.0;

    const auto first = h.tick(1);
    EXPECT_EQ(0, first.stopPushed);
    EXPECT_EQ(1, h.stop_attempts);

    // Inside the 10 s cooldown: no second attempt.
    (void)h.tick(2);
    EXPECT_EQ(1, h.stop_attempts);

    // Cooldown elapsed: retry (and now succeed).
    h.stop_push_ok = true;
    (void)h.tick(12);
    EXPECT_EQ(2, h.stop_attempts);
    EXPECT_EQ(1, h.stop_pushes);
}

TEST(GoldTrader, MissingQuoteKeepsStateAndDoesNotFire)
{
    Harness h;
    const auto id = h.open(Side::Buy, 0.01, 4000.0);
    h.price = 0.0; // no quote

    const auto result = h.tick(1);
    EXPECT_EQ(0, result.evaluated);
    EXPECT_EQ(0, result.closed);
    EXPECT_EQ(0, h.stop_pushes);
    EXPECT_FALSE(h.trader->effectiveStop(id).has_value());
}

TEST(GoldTrader, FailedCloseRetriesAfterCooldown)
{
    Harness h;
    h.open(Side::Buy, 0.01, 4000.0);
    h.price = 3991.0; // beyond the hard stop
    h.close_ok = false;

    const auto first = h.tick(1);
    EXPECT_EQ(1, first.failed);
    EXPECT_EQ(0, first.closed);

    // Inside the 10 s cooldown the supervisor must not spam close requests.
    const auto second = h.tick(2);
    EXPECT_EQ(0, second.failed);
    EXPECT_EQ(0, second.closed);

    // Cooldown elapsed: retry; the position is still there, so this succeeds
    // once the close callback is allowed to work again.
    h.close_ok = true;
    const auto third = h.tick(12);
    EXPECT_EQ(1, third.closed);
    EXPECT_EQ(1, h.closes);
}

TEST(GoldTrader, DisabledSupervisorDoesNothing)
{
    GoldTraderConfig cfg = make_gold_config();
    cfg.enabled = false;
    Harness h{ cfg };
    h.open(Side::Buy, 0.01, 4000.0);
    h.price = 3991.0;

    const auto result = h.tick(1);
    EXPECT_EQ(0, result.evaluated);
    EXPECT_EQ(0, result.closed);
    EXPECT_EQ(0, h.stop_pushes);
}

TEST(GoldTrader, RefusesMismatchedContractVolume)
{
    Harness h;
    // A 1.0 multiplier is what the engine falls back to when the CFD symbol
    // metadata load fails. Managing the position with it would turn the
    // 8 USD stop into an ~800-price stop, so the supervisor must refuse.
    const auto id = h.open(Side::Buy, 0.01, 4000.0, 0.0, 1.0);
    h.price = 3990.0;

    const auto result = h.tick(1);
    EXPECT_EQ(0, result.evaluated);
    EXPECT_EQ(0, result.closed);
    EXPECT_EQ(0, h.stop_pushes);
    EXPECT_FALSE(h.trader->effectiveStop(id).has_value());
}

TEST(GoldTrader, PollThrottleSkipsRapidTicks)
{
    Harness h;
    h.open(Side::Buy, 0.01, 4000.0);
    h.price = 3999.0;

    // The first pass always runs; the next one inside poll_interval_ms is
    // skipped, and the one after the interval evaluates again.
    const auto first = h.trader->tick(Timestamp{ std::chrono::milliseconds{ 100 } });
    EXPECT_EQ(1, first.evaluated);

    const auto throttled = h.trader->tick(Timestamp{ std::chrono::milliseconds{ 200 } });
    EXPECT_EQ(0, throttled.evaluated);

    const auto second = h.trader->tick(Timestamp{ std::chrono::milliseconds{ 1100 } });
    EXPECT_EQ(1, second.evaluated);
}

TEST(GoldTrader, BackwardClockStepDoesNotSuspendSupervisor)
{
    Harness h;
    h.open(Side::Buy, 0.01, 4000.0);
    h.price = 3999.0;

    const auto first = h.tick(10);
    EXPECT_EQ(1, first.evaluated);

    // NTP/RTC steps the wall clock back: the supervisor must evaluate again
    // instead of waiting for the clock to catch up. The stop itself is
    // already at its level, so no second exchange push is expected.
    const auto second = h.tick(-20);
    EXPECT_EQ(1, second.evaluated);
    EXPECT_EQ(1u, h.pushed_stops.size());
}

TEST(GoldTrader, BackwardClockStepRetriesCloseInsteadOfWedging)
{
    Harness h;
    h.open(Side::Buy, 0.01, 4000.0);
    h.price = 3991.0; // beyond the hard stop
    h.close_ok = false;

    const auto first = h.tick(10);
    EXPECT_EQ(1, first.failed);

    // A backward step must not push the retry cooldown further into the
    // future: retry immediately.
    const auto second = h.tick(-5);
    EXPECT_EQ(1, second.failed);
}

// ---------------------------------------------------------------------------
// State identity and re-averaging
// ---------------------------------------------------------------------------

TEST(GoldTrader, SyncIdReuseStartsFreshAfterReopen)
{
    Harness h;
    const auto id = h.sync(0.01, 4000.0, 4000.0, "111");
    ASSERT_EQ("XAUUSD_Buy_sync", id);

    h.price = 4010.0;
    (void)h.tick(1); // peak price 4010, effective stop 4005

    // The user closes the position in the app and immediately reopens the
    // same side at 4020. The sync keeps the engine-local "_sync" id but the
    // exchange position id changes.
    auto closed = h.positions.closePosition(id, 0.01, 4010.0);
    ASSERT_TRUE(closed.has_value());
    const auto reopened = h.sync(0.01, 4020.0, 4020.0, "222");
    ASSERT_EQ(id, reopened);

    h.price = 4020.0;
    const auto second = h.tick(2);

    // A stale peak would put the give-back above 4020 and close immediately.
    EXPECT_EQ(0, second.closed);
    const auto stop = h.trader->effectiveStop(id);
    ASSERT_TRUE(stop.has_value());
    EXPECT_DOUBLE_EQ(4012.0, stop.value()); // fresh hard stop: 4020 - 8
}

TEST(GoldTrader, AddToPositionKeepsPriceBasedPeak)
{
    Harness h;
    const auto id = h.sync(0.01, 4000.0, 4000.0, "111");

    h.price = 4010.0;
    (void)h.tick(1); // peak price 4010

    // Add 0.01 at 4010: the exchange re-averages the entry to 4005 and
    // reports 0.02 lots. A USD peak scaled by size would become 20 and put
    // the give-back at 4012.5 (above the mark) -> an instant bogus close.
    // Recomputing from the peak PRICE gives (4010-4005)*2 = 10 USD and a
    // give-back of 4007.5.
    h.positions.syncPositionFromExchange("XAUUSD",
        Side::Buy,
        0.02,
        4005.0,
        4010.0,
        MarketType::Cfd,
        500.0,
        MarginMode::Cross,
        100.0,
        0.0,
        0.0,
        Timestamp{},
        0.0,
        0.0,
        "111");

    const auto second = h.tick(2);
    EXPECT_EQ(0, second.closed);
    const auto stop = h.trader->effectiveStop(id);
    ASSERT_TRUE(stop.has_value());
    EXPECT_DOUBLE_EQ(4007.5, stop.value());
}

// ---------------------------------------------------------------------------
// Exchange stop reconciliation
// ---------------------------------------------------------------------------

TEST(GoldTrader, RemovedExchangeStopIsRestored)
{
    Harness h;
    const auto id = h.open(Side::Buy, 0.01, 4000.0);
    h.price = 3999.0;

    (void)h.tick(1); // pushes 3992
    ASSERT_EQ(1u, h.pushed_stops.size());

    // The user deletes the stop in the Gate app; sync reports 0.
    h.positions.updateExchangeStops(id, 0.0, 0.0);
    (void)h.tick(2);

    ASSERT_EQ(2u, h.pushed_stops.size());
    EXPECT_DOUBLE_EQ(3992.0, h.pushed_stops.back());
}

TEST(GoldTrader, LoosenedExchangeStopIsRetightened)
{
    Harness h;
    const auto id = h.open(Side::Buy, 0.01, 4000.0);
    h.price = 3999.0;

    (void)h.tick(1); // pushes 3992
    ASSERT_EQ(1u, h.pushed_stops.size());

    // The user loosens the stop to 3985; the supervisor must restore 3992.
    h.positions.updateExchangeStops(id, 3985.0, 0.0);
    (void)h.tick(2);

    ASSERT_EQ(2u, h.pushed_stops.size());
    EXPECT_DOUBLE_EQ(3992.0, h.pushed_stops.back());
}

TEST(GoldTrader, ConsecutiveTighteningsPushImmediately)
{
    Harness h;
    h.open(Side::Buy, 0.01, 4000.0);
    h.price = 3999.0;

    (void)h.tick(1); // pushes 3992
    ASSERT_EQ(1u, h.pushed_stops.size());

    // A successful push must not start the retry cooldown: break-even at
    // 4000 lands on the very next pass.
    h.price = 4003.0;
    (void)h.tick(2);

    ASSERT_EQ(2u, h.pushed_stops.size());
    EXPECT_DOUBLE_EQ(4000.0, h.pushed_stops.back());
}

// ---------------------------------------------------------------------------
// Control-plane status
// ---------------------------------------------------------------------------

TEST(GoldTrader, StatusReportsArmedConfigAndTrackedPosition)
{
    Harness h;
    const auto id = h.open(Side::Buy, 0.01, 4000.0);
    h.price = 3999.0;
    (void)h.tick(1); // publishes the status snapshot

    const auto status = h.trader->status();
    EXPECT_TRUE(status.value("armed", false));
    EXPECT_EQ("XAUUSD", status.value("symbol", ""));
    EXPECT_EQ("cfd", status.value("market_type", ""));
    ASSERT_TRUE(status.contains("config"));
    EXPECT_DOUBLE_EQ(8.0, status["config"].value("hard_stop_usd", 0.0));
    EXPECT_DOUBLE_EQ(100.0, status["config"].value("contract_volume", 0.0));

    const auto positions = status.value("tracked_positions", nlohmann::json::array());
    ASSERT_EQ(1u, positions.size());
    EXPECT_EQ(id, positions[0].value("position_id", ""));
    EXPECT_EQ("Buy", positions[0].value("side", ""));
    EXPECT_DOUBLE_EQ(0.01, positions[0].value("quantity", 0.0));
    EXPECT_DOUBLE_EQ(3999.0, positions[0].value("mark_price", 0.0));
    EXPECT_DOUBLE_EQ(-1.0, positions[0].value("unrealized_pnl", 0.0));
    EXPECT_DOUBLE_EQ(0.0, positions[0].value("peak_pnl", 0.0));
    EXPECT_DOUBLE_EQ(3992.0, positions[0].value("effective_stop", 0.0));
    EXPECT_DOUBLE_EQ(3992.0, positions[0].value("exchange_stop", 0.0));
}

TEST(GoldTrader, StatusStartsEmptyAndDropsClosedPositions)
{
    Harness h;
    const auto initial = h.trader->status();
    EXPECT_TRUE(initial.value("armed", false));
    EXPECT_TRUE(initial["tracked_positions"].empty());

    const auto id = h.open(Side::Buy, 0.01, 4000.0);
    h.price = 3999.0;
    (void)h.tick(1);

    auto closed = h.positions.closePosition(id, 0.01, 3999.0);
    ASSERT_TRUE(closed.has_value());
    (void)h.tick(2);

    EXPECT_TRUE(h.trader->status()["tracked_positions"].empty());
}
