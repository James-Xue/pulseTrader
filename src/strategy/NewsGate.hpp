#pragma once
// NewsGate.hpp — 重大消息事件闸 (major-news event gate) — pure gate logic (Layer 6)
//
// Major scheduled events (FOMC/CPI/NFP ...) must not be traded through:
// entries are blocked around the event instant and positions opened before
// the event are force-flattened ahead of it (rules doc: docs/strategies/
// iron-trader.md §4.6 — the first wired adopter; gold taskbook g7 /
// RED_WINDOW 17:45Z–18:05Z is the rule's lineage).
//
// Infrastructure spec:
//  - Scope: ANY UnifiedScalper-family strategy instance may configure preset
//    UTC windows via StrategyInstanceConfig::news_windows (TOML
//    `news_windows = [{ time = "...", close_before_min = X,
//    resume_after_min = Y }]`). Empty vector = gate off. PRESET-only — no
//    runtime/manual toggle (config edit + restart).
//  - Time: judged ONLY on candle open_time (UTC epoch ms) — no wall clock —
//    so live (closed-candle strategy loop) and backtest (ReplayDriver feeds
//    candles to onKline) behave identically. Action lands ≤ 1 bar late.
//  - Semantics per window w = {T event instant, X close_before_min,
//    Y resume_after_min}, x = X minutes, y = Y minutes:
//      blackout(t)   = ∃w: (T−x) ≤ t < (T+y)  → entries must NOT open;
//                      first openable candle has open_time ≥ T+y.
//      flattenDue(t) = holding a position entered at candle E and
//                      ∃w: E < T ∧ t ≥ T−x   → force-close now (emitted as a
//                      Flat with exit_reason = "news_blackout").
//    flattenDue is monotonic per window (no per-window "fired" latch needed —
//    the strategy state machine closes the position exactly once) and stays
//    due across candle gaps (a paused thread / data hole still flattens late
//    on resume). Windows age out naturally: a position opened at E ≥ T+y can
//    never trip that window again.
//  - Multiple windows = union (overlaps and duplicates idempotent).
//  - Live force-close EXECUTION (OrderFlowExecutor acting on the emitted
//    Flat) is a future milestone — see IronTrader.hpp lifecycle note; today
//    the gate is fully effective in backtest and at the strategy signal level.

#include "core/config.hpp"

#include <cstdint>
#include <vector>

namespace pulse::strategy
{

/// True when `open_ms` falls inside [T−X, T+Y) of any configured window —
/// entry signals must not be emitted/acted on.
[[nodiscard]] bool newsGateBlackout(const std::vector<NewsWindow> &windows,
                                    std::int64_t open_ms);

/// True when a position opened at `entry_open_ms` (candle open) must be
/// force-flattened now: ∃w: entry_open_ms < w.event_open_ms ∧
/// open_ms ≥ w.event_open_ms − X.
[[nodiscard]] bool newsGateFlattenDue(const std::vector<NewsWindow> &windows,
                                      std::int64_t entry_open_ms,
                                      std::int64_t open_ms);

} // namespace pulse::strategy
