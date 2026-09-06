#pragma once
// TimeUtil.hpp — Configurable-timezone human-readable timestamp formatting
//
// Internal timestamps are epoch nanoseconds (timezone-independent). The phone
// exchange app may display times in a different timezone than the trading
// machine (e.g. US Eastern vs Beijing), which makes cross-checking confusing.
// This utility renders ISO8601 timestamps in a configurable display timezone
// so both views can be compared at a glance:
//
//   display_timezone = "local"   — machine local time (default)
//   display_timezone = "utc"     — UTC
//   display_timezone = "-04:00"  — fixed UTC offset (e.g. US Eastern summer)
//
// Output format: "YYYY-MM-DDTHH:MM:SS.mmm±HH:MM"

#include "core/types.hpp"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace pulse
{

// ---------------------------------------------------------------------------
// DisplayTimezone — how timestamps are rendered in human-readable output
// ---------------------------------------------------------------------------
struct DisplayTimezone
{
    enum class Mode
    {
        Local, ///< Machine local timezone (default).
        Utc,   ///< UTC.
        Fixed, ///< Fixed UTC offset (offsetMinutes east of UTC).
    };

    Mode mode = Mode::Local;
    int offsetMinutes = 0; ///< Used only when mode == Fixed.

    /// Default: machine local time.
    [[nodiscard]] static constexpr DisplayTimezone local() noexcept
    {
        return {};
    }

    /// UTC display.
    [[nodiscard]] static constexpr DisplayTimezone utc() noexcept
    {
        return { Mode::Utc, 0 };
    }
};

// ---------------------------------------------------------------------------
// Parsing — "local" / "utc" / "±HH:MM"
// ---------------------------------------------------------------------------

/// Case-insensitive equality helper.
[[nodiscard]] inline bool ciEquals(std::string_view a, std::string_view b) noexcept
{
    if (a.size() != b.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        const char ca = a[i];
        const char cb = b[i];
        if (std::tolower(static_cast<unsigned char>(ca))
            != std::tolower(static_cast<unsigned char>(cb)))
        {
            return false;
        }
    }
    return true;
}

/// Parse "local" / "utc" / "±HH:MM" (sign optional, defaults to +) into a
/// DisplayTimezone. Returns nullopt on any other input.
[[nodiscard]] inline std::optional<DisplayTimezone>
parseDisplayTimezone(std::string_view sv) noexcept
{
    if (sv.empty())
    {
        return std::nullopt;
    }
    if (ciEquals(sv, "local"))
    {
        return DisplayTimezone::local();
    }
    if (ciEquals(sv, "utc"))
    {
        return DisplayTimezone::utc();
    }

    // Fixed offset: [sign]HH:MM
    std::size_t i = 0;
    int sign = 1;
    if ('+' == sv[0])
    {
        i = 1;
    }
    else if ('-' == sv[0])
    {
        i = 1;
        sign = -1;
    }
    if (sv.size() - i != 5 || ':' != sv[i + 2])
    {
        return std::nullopt;
    }
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    if (!digit(sv[i]) || !digit(sv[i + 1]) || !digit(sv[i + 3]) || !digit(sv[i + 4]))
    {
        return std::nullopt;
    }
    const int hours = (sv[i] - '0') * 10 + (sv[i + 1] - '0');
    const int minutes = (sv[i + 3] - '0') * 10 + (sv[i + 4] - '0');
    if (hours > 14 || minutes > 59)
    {
        return std::nullopt;
    }
    DisplayTimezone tz;
    tz.mode = DisplayTimezone::Mode::Fixed;
    tz.offsetMinutes = sign * (hours * 60 + minutes);
    return tz;
}

// ---------------------------------------------------------------------------
// Formatting — ISO8601 with explicit UTC offset
// ---------------------------------------------------------------------------

/// Format an epoch-millisecond timestamp as "YYYY-MM-DDTHH:MM:SS.mmm±HH:MM"
/// in the given display timezone. Always includes the offset, so the value is
/// unambiguous regardless of the viewer's (phone app's) timezone setting.
[[nodiscard]] inline std::string formatEpochMs(std::int64_t epoch_ms,
                                               const DisplayTimezone &tz) noexcept
{
    // Negative epoch (pre-1970) is not expected; clamp defensively.
    if (epoch_ms < 0)
    {
        return "1970-01-01T00:00:00.000+00:00";
    }

    const std::time_t seconds = static_cast<std::time_t>(epoch_ms / 1000);
    const int ms = static_cast<int>(epoch_ms % 1000);

    std::tm tm{};
    int offset_seconds = 0;
    switch (tz.mode)
    {
    case DisplayTimezone::Mode::Utc:
        gmtime_r(&seconds, &tm);
        break;
    case DisplayTimezone::Mode::Fixed:
    {
        const std::time_t shifted = seconds + static_cast<std::time_t>(tz.offsetMinutes) * 60;
        gmtime_r(&shifted, &tm);
        offset_seconds = tz.offsetMinutes * 60;
        break;
    }
    case DisplayTimezone::Mode::Local:
    default:
    {
        localtime_r(&seconds, &tm);
        // Local UTC offset for the instant being rendered: how far the wall
        // clock is ahead of UTC. Interpret the wall clock AS IF it were UTC
        // (timegm) and subtract the true instant. Using mktime(&tm) instead
        // would convert the wall clock back to the SAME instant (mktime
        // treats tm as local time), yielding offset 0 and labelling a
        // Beijing wall clock as "+00:00" — the old +8h display bug.
        offset_seconds = static_cast<int>(timegm(&tm) - seconds);
        break;
    }
    }

    // Decompose the offset sign-correctly (e.g. -30 min must render
    // "-00:30", not "+00:30").
    const int total_minutes = offset_seconds / 60;
    const char sign = total_minutes < 0 ? '-' : '+';
    const int hours = std::abs(total_minutes) / 60;
    const int mins = std::abs(total_minutes) % 60;

    return std::format("{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}.{:03d}{}{:02d}:{:02d}",
                       tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                       tm.tm_hour, tm.tm_min, tm.tm_sec, ms,
                       sign, hours, mins);
}

// ---------------------------------------------------------------------------
// Parsing — epoch text (bare epoch or ISO UTC)
// ---------------------------------------------------------------------------

/// Parse a time argument into UTC epoch ms: bare digits (length < 13
/// interpreted as seconds, else ms) or ISO UTC "YYYY-MM-DD" (midnight) /
/// "YYYY-MM-DDTHH:MM:SS" (a trailing "Z"/offset is naturally ignored).
/// Shared by the backtest CLI (--from/--to/--news) and the config loader
/// (news_windows "time"). Returns false and leaves out_ms untouched on any
/// parse failure — callers must treat failure as an error, never 0.
[[nodiscard]] inline bool parseEpochMsText(std::string_view text,
                                           std::int64_t &out_ms) noexcept
{
    if (text.empty())
    {
        return false;
    }

    // Pure numeric → epoch (seconds if < 1e12, else ms).
    bool all_digits = true;
    for (const char c : text)
    {
        all_digits = all_digits && (c >= '0' && c <= '9');
    }
    if (all_digits)
    {
        // stoll on a string_view: hand-craft the value to stay noexcept.
        std::int64_t v = 0;
        for (const char c : text)
        {
            v = v * 10 + static_cast<std::int64_t>(c - '0');
        }
        out_ms = (v < 1'000'000'000'000LL) ? v * 1000 : v;
        return true;
    }

    // ISO UTC. Accept "YYYY-MM-DD" (midnight) and "YYYY-MM-DDTHH:MM:SS".
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    const std::string tmp{ text };
    if (6 > std::sscanf(tmp.c_str(), "%d-%d-%dT%d:%d:%d",
                        &year, &month, &day, &hour, &minute, &second)
        && 3 > std::sscanf(tmp.c_str(), "%d-%d-%d", &year, &month, &day))
    {
        return false;
    }

    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    tm.tm_isdst = 0;

    const std::time_t secs = timegm(&tm);
    if (-1 == secs)
    {
        return false;
    }
    out_ms = static_cast<std::int64_t>(secs) * 1000;
    return true;
}

/// Format the project's nanosecond Timestamp as an ISO8601 string.
[[nodiscard]] inline std::string
formatIsoTimestamp(const Timestamp &ts, const DisplayTimezone &tz) noexcept
{
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        ts.time_since_epoch()).count();
    return formatEpochMs(ms, tz);
}

} // namespace pulse
