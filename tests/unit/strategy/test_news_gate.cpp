// test_news_gate.cpp — Unit tests for the 重大消息事件闸 pure gate logic
// (NewsGate.hpp). Pins the boundary semantics locked in the docs
// (docs/strategies/iron-trader.md §4.6):
//   blackout(t)   = ∃w: (T−X) ≤ t < (T+Y)          → entries blocked
//   flattenDue(t) = ∃w: entry_open < T ∧ t ≥ T−X   → pre-event position closes
// All times are raw UTC epoch ms; minutes are exact multiples of 60'000.

#include "strategy/NewsGate.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

using namespace pulse;
using namespace pulse::strategy;

namespace
{

constexpr std::int64_t kMinMs = 60'000;
constexpr std::int64_t T = 1'800'000'000'000LL;  // Fixed event instant (ms).

NewsWindow win(std::int64_t event_ms, double x = 15.0, double y = 15.0)
{
    NewsWindow w;
    w.event_open_ms = event_ms;
    w.close_before_min = x;
    w.resume_after_min = y;
    return w;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// blackout — exact window boundaries
// ---------------------------------------------------------------------------

TEST(NewsGate, BlackoutExactBoundaries)
{
    const auto windows = std::vector<NewsWindow>{ win(T, 15.0, 15.0) };

    EXPECT_FALSE(newsGateBlackout(windows, T - 15 * kMinMs - 1));  // just before
    EXPECT_TRUE(newsGateBlackout(windows, T - 15 * kMinMs));       // block start
    EXPECT_TRUE(newsGateBlackout(windows, T));                     // the event
    EXPECT_TRUE(newsGateBlackout(windows, T + 15 * kMinMs - 1));   // pre-end
    EXPECT_FALSE(newsGateBlackout(windows, T + 15 * kMinMs));      // resume
}

TEST(NewsGate, XZeroStartsBlockAtEvent)
{
    const auto windows = std::vector<NewsWindow>{ win(T, 0.0, 15.0) };
    EXPECT_FALSE(newsGateBlackout(windows, T - 1));
    EXPECT_TRUE(newsGateBlackout(windows, T));
}

TEST(NewsGate, YZeroClearsAtEvent)
{
    const auto windows = std::vector<NewsWindow>{ win(T, 15.0, 0.0) };
    EXPECT_TRUE(newsGateBlackout(windows, T - 15 * kMinMs));
    EXPECT_TRUE(newsGateBlackout(windows, T - 1));   // window [T−X, T)
    EXPECT_FALSE(newsGateBlackout(windows, T));
}

TEST(NewsGate, EmptyWindowsNeverBlock)
{
    const std::vector<NewsWindow> windows;
    EXPECT_FALSE(newsGateBlackout(windows, T));
    EXPECT_FALSE(newsGateBlackout(windows, 0));
    EXPECT_FALSE(newsGateFlattenDue(windows, T - kMinMs, T));
}

TEST(NewsGate, MultiWindowUnionOverlapsAndOrderIndependent)
{
    const std::int64_t T2 = T + 10 * kMinMs;   // Overlaps window 1's tail.
    const std::int64_t T3 = T + 40 * kMinMs;   // Disjoint third window.

    // Shuffled order on purpose — union semantics must not care.
    const auto windows = std::vector<NewsWindow>{ win(T3, 5.0, 5.0),
                                                  win(T, 15.0, 15.0),
                                                  win(T2, 15.0, 15.0) };

    EXPECT_TRUE(newsGateBlackout(windows, T));                       // w1
    EXPECT_TRUE(newsGateBlackout(windows, T2));                      // overlap band
    EXPECT_TRUE(newsGateBlackout(windows, T + 15 * kMinMs - 1));     // w1 tail
    EXPECT_TRUE(newsGateBlackout(windows, T + 24 * kMinMs));         // w2 interior
    EXPECT_FALSE(newsGateBlackout(windows, T + 25 * kMinMs));        // w2 end (excl.)
    EXPECT_TRUE(newsGateBlackout(windows, T3 - 5 * kMinMs));         // w3 start
    EXPECT_FALSE(newsGateBlackout(windows, T + 30 * kMinMs));        // w1/w2 gap
    EXPECT_FALSE(newsGateBlackout(windows, T3 + 5 * kMinMs));        // past w3

    // Duplicates are idempotent.
    const auto dup = std::vector<NewsWindow>{ win(T, 15.0, 15.0),
                                              win(T, 15.0, 15.0) };
    EXPECT_EQ(newsGateBlackout(dup, T - 15 * kMinMs),
              newsGateBlackout(windows, T - 15 * kMinMs));
}

// ---------------------------------------------------------------------------
// flattenDue — pre-event positions flatten from T−X, no earlier, and even late
// ---------------------------------------------------------------------------

TEST(NewsGate, FlattenDueFromPreCloseAndNotBefore)
{
    const auto windows = std::vector<NewsWindow>{ win(T, 10.0, 10.0) };
    const std::int64_t entry = T - 60 * kMinMs;   // Position opened 1h before.

    EXPECT_FALSE(newsGateFlattenDue(windows, entry, T - 10 * kMinMs - 1));
    EXPECT_TRUE(newsGateFlattenDue(windows, entry, T - 10 * kMinMs));
    EXPECT_TRUE(newsGateFlattenDue(windows, entry, T));              // event itself
    // Still due long AFTER the window: a paused thread / data hole must also
    // flatten late on resume (the gate is membership-in-time, not a latch).
    EXPECT_TRUE(newsGateFlattenDue(windows, entry, T + 60 * kMinMs));
}

TEST(NewsGate, FlattenDueNeverForPostEventEntry)
{
    const auto windows = std::vector<NewsWindow>{ win(T, 10.0, 10.0) };
    // Entry after T+Y can never trip this window (blackout expired).
    const std::int64_t entry = T + 30 * kMinMs;
    EXPECT_FALSE(newsGateFlattenDue(windows, entry, T + 30 * kMinMs));
    EXPECT_FALSE(newsGateFlattenDue(windows, entry, T + 10 * kMinMs + 1));
    // ...though the entry candle itself may fall inside a *later* window.
    const auto later = std::vector<NewsWindow>{ win(T + 60 * kMinMs, 5.0, 5.0) };
    EXPECT_FALSE(newsGateFlattenDue(later, entry, T + 30 * kMinMs));
    EXPECT_TRUE(newsGateFlattenDue(later, entry, T + 60 * kMinMs - 5 * kMinMs));
}

TEST(NewsGate, FlattenDueWindowsIndependent)
{
    const auto windows = std::vector<NewsWindow>{ win(T, 10.0, 10.0),
                                                  win(T + 120 * kMinMs, 10.0, 10.0) };
    // Entry AFTER w1's event window (E = T+30m > T1+Y1) but before w2.
    const std::int64_t entry = T + 30 * kMinMs;
    // w1 can never flatten a post-event entry (E < T1 is false)...
    EXPECT_FALSE(newsGateFlattenDue(windows, entry, T + 60 * kMinMs));
    // ...and w2 applies on its own schedule: nothing until T2−X...
    EXPECT_FALSE(newsGateFlattenDue(windows, entry,
                                    T + 120 * kMinMs - 10 * kMinMs - 1));
    EXPECT_TRUE(newsGateFlattenDue(windows, entry,
                                   T + 120 * kMinMs - 10 * kMinMs));
}
