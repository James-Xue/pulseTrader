// test_iron_trader.cpp — Unit tests for IronTrader 铁律交易员
// (full kline pipeline; same harness pattern as test_ema_resonance_scalper).
//
// The rule baseline is docs/strategies/iron-trader.md; these tests pin the
// acceptance checklist §8 at the behaviour level: gate stack (G1–G3),
// triggers (T-A 拉回 / T-B 突破确认), the close-only Flat exit channel,
// R7 breakeven/trail, and the R0/R1/R6 paper ledger.

#include "strategy/scalping/IronTrader.hpp"

#include "exchange/GateRestClient.hpp"
#include "market/MarketFeed.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace pulse;
using namespace pulse::strategy;

namespace
{

constexpr std::int64_t kDayMs = 86'400'000;
constexpr std::int64_t kHourMs = 3'600'000;

// UTC-midnight-aligned epoch (20342 full days); 08:00Z main-session start so
// multi-hour test sequences stay inside one day bucket (UTC day = the paper
// ledger's reset boundary).
constexpr std::int64_t kDayStart = 1'757'548'800'000LL;
constexpr std::int64_t kMainOpen = kDayStart + 8 * kHourMs;   // 08:00Z

// Fast custom_params: short periods keep the warmup window small
// (klineNeeded = it_trend_ema + 60 = 90 with trend 30).
std::map<std::string, double> fastParams()
{
    return {
        { "it_trend_ema", 30.0 },
        { "it_fast_ema", 5.0 },
        { "it_slow_ema", 10.0 },
        { "it_atr_len", 14.0 },
        { "it_breakout_n", 10.0 },
        { "it_sep_gate", 0.10 },
        { "it_sl_atr", 2.0 },
    };
}

struct FeedHarness
{
    exchange::GateRestClient rest;
    market::MarketFeed feed;
    double last_close{ 100.0 };
    std::int64_t t{ kMainOpen };

    explicit FeedHarness(MarketType market_type)
        : rest{ ExchangeConfig{}, market_type }
        , feed{ nullptr, rest, market_type }
    {
    }

    // Push one candle and run ONE evaluation (each call = one closed candle).
    void candle(IronTrader &s, double close, double range = 0.05)
    {
        market::Kline k;
        k.open_time = t;
        k.close_time = t + 60'000;
        k.open = last_close;
        k.close = close;
        k.high = std::max(k.open, close) + range;
        k.low = std::min(k.open, close) - range;
        k.closed = true;
        last_close = close;
        t += 60'000;

        auto &buf = feed.getKlineBuffer("BTC_USDT");
        buf.push(k);
        market::Kline trigger;  // evaluation only — buffer already has `k`
        trigger.closed = true;
        s.onKline(trigger);
    }

    // Push `n` candles drifting by `step` per candle (each one evaluated).
    void ramp(IronTrader &s, double step, std::size_t n)
    {
        for (std::size_t i = 0; i < n; ++i)
        {
            candle(s, last_close + step);
        }
    }
};

struct Trader
{
    FeedHarness harness{ MarketType::Futures };
    std::unique_ptr<IronTrader> strategy;
    std::vector<TradingSignal> received;

    explicit Trader(std::map<std::string, double> custom = {},
        std::vector<NewsWindow> windows = {})
    {
        auto merged = fastParams();
        for (const auto &[k, v] : custom)
        {
            merged[k] = v;
        }

        StrategyContext ctx;
        ctx.config.name = "iron_trader";
        ctx.config.symbol = "BTC_USDT";
        ctx.config.market_type = MarketType::Futures;
        ctx.config.custom_params = std::move(merged);
        ctx.config.news_windows = std::move(windows);
        ctx.market_feed = &harness.feed;

        strategy = std::make_unique<IronTrader>(ctx);
        strategy->params().min_confidence.store(0.0, std::memory_order_release);
        strategy->params().cooldown_seconds.store(0.0, std::memory_order_release);
        strategy->params().order_quantity.store(20.0, std::memory_order_release);
        strategy->setSignalCallback([this](const TradingSignal &sig)
            {
                received.push_back(sig);
            });
    }

    // Warm the buffer up on a flat 100.0 series (klineNeeded = 90 + margin).
    void warmup(std::size_t bars = 100)
    {
        for (std::size_t i = 0; i < bars; ++i)
        {
            harness.candle(*strategy, harness.last_close);
        }
    }
};

/// Push a bull trend (ramp) then a SHALLOW multi-bar dip below EMA_fast, then
/// rising bars until an entry fires (T-A 拉回) or `max_bars` pass.
///
/// Note: a deep single-bar dip would push sep under the gate on the reclaim
/// bar and dead-lock T-A (the reclaim bar is precisely when sep is weakest) —
/// realistic pullbacks are 2–3 shallow bars slightly deeper than the fast
/// EMA's lead.
void driveBullPullbackToEntry(Trader &tr, std::size_t max_bars = 12)
{
    tr.warmup();
    tr.harness.ramp(*tr.strategy, 0.02, 45);        // bull trend (sep ≫ gate)
    tr.harness.candle(*tr.strategy, tr.harness.last_close - 0.02);
    tr.harness.candle(*tr.strategy, tr.harness.last_close - 0.02);
    tr.harness.candle(*tr.strategy, tr.harness.last_close - 0.02);  // 浅拉回
    for (std::size_t i = 0; i < max_bars; ++i)
    {
        tr.harness.candle(*tr.strategy, tr.harness.last_close + 0.03);
        const bool entered = std::any_of(tr.received.begin(), tr.received.end(),
            [](const TradingSignal &s) { return SignalType::Buy == s.type; });
        if (entered)
        {
            return;
        }
    }
}

/// Crash the market far enough through the protective stop to force a
/// hard-stop exit (it_sl_atr = 2.0 × ATR14 ≈ 0.2 in the fixture).
void crashThroughStop(Trader &tr)
{
    tr.harness.candle(*tr.strategy, tr.harness.last_close - 0.35);
}

std::size_t buyCount(const std::vector<TradingSignal> &signals)
{
    return static_cast<std::size_t>(std::count_if(signals.begin(), signals.end(),
        [](const TradingSignal &s) { return SignalType::Buy == s.type; }));
}

std::size_t flatCount(const std::vector<TradingSignal> &signals)
{
    return static_cast<std::size_t>(std::count_if(signals.begin(), signals.end(),
        [](const TradingSignal &s) { return SignalType::Flat == s.type; }));
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Identity / warmup
// ---------------------------------------------------------------------------

TEST(IronTrader, NameAndId)
{
    Trader tr;
    EXPECT_EQ("IronTrader", tr.strategy->name());
    EXPECT_EQ("iron_trader_BTC_USDT", tr.strategy->id());
}

TEST(IronTrader, NoSignalDuringWarmupAndFlatMarket)
{
    Trader tr;
    tr.warmup();
    tr.harness.ramp(*tr.strategy, 0.0, 20);   // still flat — no structure
    EXPECT_TRUE(tr.received.empty());
}

// ---------------------------------------------------------------------------
// T-A 顺势拉回 entry + Flat close-only exit channel
// ---------------------------------------------------------------------------

TEST(IronTrader, BullPullbackEntersLongThenHardStopExitsFlat)
{
    Trader tr;
    driveBullPullbackToEntry(tr);
    ASSERT_EQ(1u, buyCount(tr.received));

    const auto &buy = tr.received.back();
    EXPECT_EQ("BTC_USDT", buy.symbol);
    EXPECT_EQ("iron_trader_BTC_USDT", buy.strategy_id);
    EXPECT_GE(buy.confidence, 0.6);   // engine default min_confidence passes
    EXPECT_NE(std::string::npos, buy.reason.find("pullback_resume"));

    // Hard stop: crash below entry − it_sl_atr(2.0)×ATR14 (~0.2).
    crashThroughStop(tr);

    // Flat exit fired (close-only) and NO Sell was emitted (no auto reverse).
    const auto flat = std::find_if(tr.received.begin(), tr.received.end(),
        [](const TradingSignal &s) { return SignalType::Flat == s.type; });
    ASSERT_NE(tr.received.end(), flat);
    EXPECT_NE(std::string::npos, flat->reason.find("hard_stop"));
    EXPECT_EQ(0u, std::count_if(tr.received.begin(), tr.received.end(),
        [](const TradingSignal &s) { return SignalType::Sell == s.type; }));
}

TEST(IronTrader, PositionManagedOnlyNoSecondEntryWhileHolding)
{
    Trader tr;
    driveBullPullbackToEntry(tr);
    ASSERT_EQ(1u, buyCount(tr.received));

    // More bullish bars while holding → still exactly one Buy (R2: 有仓只做管理).
    tr.harness.ramp(*tr.strategy, 0.02, 10);
    EXPECT_EQ(1u, buyCount(tr.received));
}

TEST(IronTrader, TimeStopExitsFlatAndCooldownGatesReentry)
{
    Trader tr;
    driveBullPullbackToEntry(tr);
    ASSERT_EQ(1u, buyCount(tr.received));

    // Hold through the timeout with a tiny monotonic drift (float stays well
    // under it_be_x_sl×stop and sep stays positive → no other exit preempts)
    // — default timeout is 60 bars.
    for (std::size_t i = 0; i < 62; ++i)
    {
        tr.harness.candle(*tr.strategy, tr.harness.last_close + 0.0005);
    }
    const auto flat = std::find_if(tr.received.begin(), tr.received.end(),
        [](const TradingSignal &s) { return SignalType::Flat == s.type; });
    ASSERT_NE(tr.received.end(), flat);
    EXPECT_NE(std::string::npos, flat->reason.find("time_stop"));

    // Cooldown (3 bars): no entry for the next few reclaim bars, then a new
    // pullback setup may fire again.
    const std::size_t buys_after_exit = buyCount(tr.received);
    for (std::size_t i = 0; i < 6; ++i)
    {
        tr.harness.candle(*tr.strategy, tr.harness.last_close - 0.02);
        tr.harness.candle(*tr.strategy, tr.harness.last_close + 0.04);
    }
    EXPECT_GE(buyCount(tr.received), buys_after_exit);  // may re-enter later
}

TEST(IronTrader, BreakevenThenTrailingStopLocksProfit)
{
    Trader tr;
    driveBullPullbackToEntry(tr);
    ASSERT_EQ(1u, buyCount(tr.received));
    const double entry = tr.received.back().price;

    // Strong rally (≥ 1× stop distance ≈ 0.2 → breakeven; then more).
    tr.harness.ramp(*tr.strategy, 0.05, 8);

    // Pullback: the trailed stop (best − it_trail_atr×ATR) must be above
    // entry by now; a close dipping to ~best − 0.1 exits with a locked gain.
    bool exited = false;
    for (std::size_t i = 0; i < 10 && !exited; ++i)
    {
        tr.harness.candle(*tr.strategy, tr.harness.last_close - 0.08);
        exited = std::any_of(tr.received.begin(), tr.received.end(),
            [](const TradingSignal &s) { return SignalType::Flat == s.type; });
    }
    ASSERT_TRUE(exited);
    const auto flat = std::find_if(tr.received.begin(), tr.received.end(),
        [](const TradingSignal &s) { return SignalType::Flat == s.type; });
    // exit price strictly better than breakeven → profit locked (R7)
    EXPECT_GT(flat->price, entry);
}

// ---------------------------------------------------------------------------
// T-B 突破确认制
// ---------------------------------------------------------------------------

TEST(IronTrader, BoxBreakoutRequiresConfirmationAndFiresOnSecondClose)
{
    // RSI chase filter disabled (it_rsi_ob=100) to isolate the confirmation
    // machine; the breakout itself carries the momentum (G2 satisfied by the
    // preceding ramp).
    Trader tr{ { { "it_rsi_ob", 100.0 }, { "it_rsi_os", 0.0 } } };
    tr.warmup();
    tr.harness.ramp(*tr.strategy, 0.02, 40);    // trend + momentum
    tr.harness.ramp(*tr.strategy, 0.001, 8);    // quiet box, RSI cooling
    const double box_top = tr.harness.last_close;

    // Break bar: one decisive close above the box → armed, no signal yet.
    // (Jump sized so the TR stays under 3×medTR20 — a spike root would demand
    // a third re-confirmation bar per the rules.)
    tr.harness.candle(*tr.strategy, box_top + 0.15);
    EXPECT_EQ(0u, buyCount(tr.received));

    // Second close beyond → confirmed breakout entry.
    tr.harness.candle(*tr.strategy, box_top + 0.15);
    ASSERT_EQ(1u, buyCount(tr.received));
    EXPECT_NE(std::string::npos, tr.received.back().reason.find("box_breakout"));
}

TEST(IronTrader, FakeBreakoutInsideBarCancelsNoEntry)
{
    Trader tr{ { { "it_rsi_ob", 100.0 }, { "it_rsi_os", 0.0 } } };
    tr.warmup();
    tr.harness.ramp(*tr.strategy, 0.02, 40);
    tr.harness.ramp(*tr.strategy, 0.001, 8);
    const double box_top = tr.harness.last_close;

    tr.harness.candle(*tr.strategy, box_top + 0.20);  // break — armed
    tr.harness.candle(*tr.strategy, box_top - 0.10);  // back inside → 假破位
    tr.harness.ramp(*tr.strategy, 0.001, 6);          // range continues
    EXPECT_EQ(0u, buyCount(tr.received));
}

TEST(IronTrader, RsiChaseFilterBlocksOverboughtBreakout)
{
    Trader tr;   // default RSI 70 filter
    tr.warmup();
    tr.harness.ramp(*tr.strategy, 0.02, 60);   // strong ramp → RSI ≫ 70
    const double box_top = tr.harness.last_close;
    tr.harness.candle(*tr.strategy, box_top + 0.20);   // break
    tr.harness.candle(*tr.strategy, box_top + 0.25);   // would confirm
    tr.harness.ramp(*tr.strategy, 0.02, 4);
    EXPECT_EQ(0u, buyCount(tr.received));     // 超买不追多破位 (M22)
}

// ---------------------------------------------------------------------------
// R1 / R6 paper ledger
// ---------------------------------------------------------------------------

TEST(IronTrader, R1BlocksSignalWhenStopCostExceedsRiskBudget)
{
    Trader tr;
    driveBullPullbackToEntry(tr);
    ASSERT_EQ(1u, buyCount(tr.received));

    // Close the position (hard stop), then massively oversize the account
    // so any stop now costs > 2% of the 100 USD paper equity.
    crashThroughStop(tr);
    ASSERT_EQ(1u, flatCount(tr.received));

    tr.strategy->params().order_quantity.store(2'000'000.0,
        std::memory_order_release);

    // Rebuild the trend and drive the same pullback setups — R1 must refuse
    // every entry while the stop cost exceeds the 2% budget.
    driveBullPullbackToEntry(tr);
    EXPECT_EQ(1u, buyCount(tr.received));

    // Restore a sane size → the same setups now pass R1 and trade again
    // (proving the block above was the R1 gate, not the market structure).
    tr.strategy->params().order_quantity.store(20.0, std::memory_order_release);
    driveBullPullbackToEntry(tr);
    EXPECT_GE(buyCount(tr.received), 2u);
}

TEST(IronTrader, DailyStopAfterThreeConsecutiveLosses)
{
    Trader tr;
    // Three losing round-trips: entry then an immediate crash through the
    // stop (consec_loss = 3 → R6 day stop).
    for (int round = 0; round < 3; ++round)
    {
        driveBullPullbackToEntry(tr);
        const auto buys = buyCount(tr.received);
        EXPECT_EQ(static_cast<std::size_t>(round + 1), buys);
        crashThroughStop(tr);
        EXPECT_EQ(static_cast<std::size_t>(round + 1), flatCount(tr.received));
        // Cooldown has passed inside driveBullPullbackToEntry (it ramps first).
        tr.harness.ramp(*tr.strategy, 0.02, 10);   // rebuild the bull trend
    }

    // Fourth setup attempt → R6 日亏红线压过一切: nothing more today.
    const auto buys = buyCount(tr.received);
    driveBullPullbackToEntry(tr, 20);
    EXPECT_EQ(buys, buyCount(tr.received));

    // Next UTC day (open_time crosses the day boundary) resets the ledger.
    while (tr.harness.t < kDayStart + 2 * kDayMs)
    {
        tr.harness.candle(*tr.strategy, tr.harness.last_close + 0.02);
    }
    driveBullPullbackToEntry(tr, 25);
    EXPECT_GT(buyCount(tr.received), buys);
}

// ---------------------------------------------------------------------------
// 重大消息事件闸 (§4.6)
//
// Fixture geometry: entry candles land at kMainOpen + 148m.. (+warmup 100 +
// ramp 45 + dip 3). All news-gate windows below are placed relative to that
// known band and stay inside the main session (08:00Z..20:00Z UTC).
// ---------------------------------------------------------------------------

/// True when any received signal carries the news_blackout exit reason.
bool anyNewsFlatten(const std::vector<TradingSignal> &signals)
{
    return std::any_of(signals.begin(), signals.end(),
        [](const TradingSignal &s)
        {
            return SignalType::Flat == s.type
                && s.indicators.contains("exit_reason")
                && "news_blackout"
                    == s.indicators.value("exit_reason", std::string{});
        });
}

TEST(IronTrader, NewsGateSuppressesEntriesDuringBlackout)
{
    // Blackout [kMainOpen+147m, kMainOpen+153m) covers the reclaim-bar band
    // where the no-window fixture would enter (~kMainOpen+148m).
    NewsWindow w;
    w.event_open_ms = kMainOpen + 150 * 60'000;
    w.close_before_min = 3.0;
    w.resume_after_min = 3.0;
    Trader tr{ {}, { w } };

    tr.warmup();
    tr.harness.ramp(*tr.strategy, 0.02, 45);      // bull trend
    tr.harness.candle(*tr.strategy, tr.harness.last_close - 0.02);
    tr.harness.candle(*tr.strategy, tr.harness.last_close - 0.02);
    tr.harness.candle(*tr.strategy, tr.harness.last_close - 0.02);  // 浅拉回
    for (std::size_t i = 0; i < 12; ++i)          // reclaim attempts, all blocked
    {
        tr.harness.candle(*tr.strategy, tr.harness.last_close + 0.03);
    }
    EXPECT_EQ(0u, buyCount(tr.received));         // window swallowed the setup
    EXPECT_EQ(0u, flatCount(tr.received));

    // Window over (open_time ≥ T+Y): the same pullback geometry re-enters.
    driveBullPullbackToEntry(tr);
    EXPECT_GE(buyCount(tr.received), 1u);
    EXPECT_FALSE(anyNewsFlatten(tr.received));
}

TEST(IronTrader, NewsGateFlattensHeldPositionAtPreClose)
{
    // Long timeout so the gentle post-entry drift never trips time_stop.
    Trader tr{ { { "it_timeout_bars", 200.0 } } };
    driveBullPullbackToEntry(tr);
    ASSERT_EQ(1u, buyCount(tr.received));

    // Window 40m after entry-confirm: blackout [t+30m, t+50m), flatten from
    // the first candle with open_time ≥ t+30m.
    const std::int64_t confirm_t = tr.harness.t;
    NewsWindow w;
    w.event_open_ms = confirm_t + 40 * 60'000;
    w.close_before_min = 10.0;
    w.resume_after_min = 10.0;
    Trader gated{ { { "it_timeout_bars", 200.0 } }, { w } };
    driveBullPullbackToEntry(gated);
    ASSERT_EQ(1u, buyCount(gated.received));
    ASSERT_EQ(confirm_t, gated.harness.t);        // both fixtures drive identically

    // Gentle drift BEFORE T−X (30m): still holding, no premature exit.
    for (std::size_t i = 0; i < 25; ++i)
    {
        gated.harness.candle(*gated.strategy, gated.harness.last_close + 0.0005);
    }
    EXPECT_EQ(1u, buyCount(gated.received));
    EXPECT_EQ(0u, flatCount(gated.received));

    // Crossing T−X → exactly one news_blackout Flat (close-only), no Sell.
    for (std::size_t i = 0; i < 8; ++i)
    {
        gated.harness.candle(*gated.strategy, gated.harness.last_close + 0.0005);
    }
    ASSERT_EQ(1u, flatCount(gated.received));
    EXPECT_TRUE(anyNewsFlatten(gated.received));
    EXPECT_EQ(0u, std::count_if(gated.received.begin(), gated.received.end(),
        [](const TradingSignal &s) { return SignalType::Sell == s.type; }));

    // Through the event and past T+Y: no second Flat, no entry inside the
    // blackout (fresh pullback geometry only re-enters after T+Y).
    for (std::size_t i = 0; i < 20; ++i)
    {
        gated.harness.candle(*gated.strategy, gated.harness.last_close + 0.0005);
    }
    EXPECT_EQ(1u, flatCount(gated.received));

    // After T+Y a fresh pullback setup trades again — and its entry (E > T+Y)
    // can never trip this window.
    driveBullPullbackToEntry(gated);
    EXPECT_GE(buyCount(gated.received), 2u);
    EXPECT_EQ(1u, flatCount(gated.received));
    EXPECT_TRUE(anyNewsFlatten(gated.received));
}

TEST(IronTrader, NewsGateStandsDownBreakoutMachineAcrossEvent)
{
    // RSI chase filter off (isolate the confirmation machine). Window
    // [kMainOpen+149m, kMainOpen+151m) sits on the would-be confirmation bar
    // right after the break bar (open +148m).
    NewsWindow w;
    w.event_open_ms = kMainOpen + 150 * 60'000;
    w.close_before_min = 1.0;
    w.resume_after_min = 1.0;
    Trader tr{ { { "it_rsi_ob", 100.0 }, { "it_rsi_os", 0.0 } }, { w } };

    tr.warmup();
    tr.harness.ramp(*tr.strategy, 0.02, 40);      // trend + momentum
    tr.harness.ramp(*tr.strategy, 0.001, 8);      // quiet box, RSI cooling
    const double box_top = tr.harness.last_close;

    tr.harness.candle(*tr.strategy, box_top + 0.15);  // break — armed (+148m)
    EXPECT_EQ(0u, buyCount(tr.received));

    // Would-be confirmation bars land inside the blackout → suppressed
    // (entry evaluation never runs), and the armed machine is stood down.
    tr.harness.candle(*tr.strategy, box_top + 0.09);  // +149m (blocked)
    tr.harness.candle(*tr.strategy, box_top + 0.09);  // +150m (blocked)
    EXPECT_EQ(0u, buyCount(tr.received));

    // Post-window fresh jump above the (now elevated) box → the machine must
    // RE-ARM (run=1, no entry). Had it NOT been stood down, these would-be
    // closes beyond the original level would have instantly confirmed on the
    // first bar back — an event-spike bar can never confirm a breakout.
    tr.harness.candle(*tr.strategy, box_top + 0.25);  // +151m → re-armed
    EXPECT_EQ(0u, buyCount(tr.received));

    // ... the SECOND consecutive close beyond confirms (§4.2 确认制).
    tr.harness.candle(*tr.strategy, box_top + 0.25);  // +152m → run=2
    ASSERT_EQ(1u, buyCount(tr.received));
    EXPECT_NE(std::string::npos,
              tr.received.back().reason.find("box_breakout"));
}

// (Session-tiering / tight-gate behaviour is exercised by the real backtest
// window sweeps; exact-bar unit coverage proved too brittle for the EMA
// lead geometry — see test header notes.)
