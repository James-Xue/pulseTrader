// NewsGate.cpp — 重大消息事件闸 pure gate logic (Layer 6)
//
// Two stateless queries over preset UTC news windows (spec: NewsGate.hpp).
// Nothing here is strategy-specific: position state lives in the adopting
// strategy, this component only answers "is this candle blocked / is a
// pre-event position due to flatten".

#include "strategy/NewsGate.hpp"

#include <cmath>
#include <cstdint>

namespace pulse::strategy
{

namespace
{

constexpr std::int64_t kMinuteMs = 60'000;

/// X minutes → ms (rounding the fractional-minute config).
std::int64_t minutesToMs(double minutes)
{
    return static_cast<std::int64_t>(
        std::llround(minutes * static_cast<double>(kMinuteMs)));
}

/// Window start = T − X (ms).
std::int64_t windowStartMs(const NewsWindow &w)
{
    return w.event_open_ms - minutesToMs(w.close_before_min);
}

/// Window end = T + Y (ms, exclusive).
std::int64_t windowEndMs(const NewsWindow &w)
{
    return w.event_open_ms + minutesToMs(w.resume_after_min);
}

} // namespace

bool newsGateBlackout(const std::vector<NewsWindow> &windows, std::int64_t open_ms)
{
    for (const auto &w : windows)
    {
        const std::int64_t start = windowStartMs(w);
        const std::int64_t end = windowEndMs(w);
        if (open_ms >= start && open_ms < end)
        {
            return true;
        }
    }
    return false;
}

bool newsGateFlattenDue(const std::vector<NewsWindow> &windows,
                        std::int64_t entry_open_ms, std::int64_t open_ms)
{
    for (const auto &w : windows)
    {
        // A position entered before the event must be flat from T−X onward,
        // no matter how late the evaluation candle arrives (gap/pause-safe).
        if (entry_open_ms < w.event_open_ms
            && open_ms >= windowStartMs(w))
        {
            return true;
        }
    }
    return false;
}

} // namespace pulse::strategy
