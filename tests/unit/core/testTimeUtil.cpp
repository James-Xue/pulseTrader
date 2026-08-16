// testTimeUtil.cpp — Unit tests for pulse/TimeUtil.hpp
//
// Test coverage:
//   1. parseDisplayTimezone — local / utc / fixed offsets (valid + invalid)
//   2. formatEpochMs — ISO8601 output for fixed offsets, UTC, and local
//
// Fixed-offset cases are exact; local depends on the machine's TZ, so it is
// only checked structurally (ISO shape + ±HH:MM suffix).

#include <gtest/gtest.h>

#include "core/TimeUtil.hpp"

using namespace pulse;

// ---------------------------------------------------------------------------
// parseDisplayTimezone
// ---------------------------------------------------------------------------

TEST(DisplayTimezone, KeywordsCaseInsensitive)
{
    EXPECT_TRUE(parseDisplayTimezone("local"));
    EXPECT_TRUE(parseDisplayTimezone("LOCAL"));
    EXPECT_TRUE(parseDisplayTimezone("Local"));
    EXPECT_TRUE(parseDisplayTimezone("utc"));
    EXPECT_TRUE(parseDisplayTimezone("UTC"));

    const auto tz = parseDisplayTimezone("LOCAL");
    ASSERT_TRUE(tz);
    EXPECT_EQ(tz->mode, DisplayTimezone::Mode::Local);
}

TEST(DisplayTimezone, FixedOffsetParsing)
{
    // Positive offset.
    const auto plus = parseDisplayTimezone("+08:00");
    ASSERT_TRUE(plus);
    EXPECT_EQ(plus->mode, DisplayTimezone::Mode::Fixed);
    EXPECT_EQ(plus->offsetMinutes, 8 * 60);

    // Negative offset (US Eastern summer time).
    const auto minus = parseDisplayTimezone("-04:00");
    ASSERT_TRUE(minus);
    EXPECT_EQ(minus->mode, DisplayTimezone::Mode::Fixed);
    EXPECT_EQ(minus->offsetMinutes, -4 * 60);

    // Optional sign defaults to positive.
    const auto bare = parseDisplayTimezone("05:30");
    ASSERT_TRUE(bare);
    EXPECT_EQ(bare->offsetMinutes, 5 * 60 + 30);

    // Sub-hour offsets.
    const auto half = parseDisplayTimezone("+05:45");
    ASSERT_TRUE(half);
    EXPECT_EQ(half->offsetMinutes, 5 * 60 + 45);
}

TEST(DisplayTimezone, InvalidInputRejected)
{
    EXPECT_FALSE(parseDisplayTimezone(""));
    EXPECT_FALSE(parseDisplayTimezone("beijing"));
    EXPECT_FALSE(parseDisplayTimezone("+08"));        // missing :MM
    EXPECT_FALSE(parseDisplayTimezone("8:00"));       // missing leading 0
    EXPECT_FALSE(parseDisplayTimezone("+08:0"));      // short
    EXPECT_FALSE(parseDisplayTimezone("+8:00"));      // single digit hour
    EXPECT_FALSE(parseDisplayTimezone("+15:00"));     // hour out of range
    EXPECT_FALSE(parseDisplayTimezone("-04:61"));     // minute out of range
    EXPECT_FALSE(parseDisplayTimezone("+0a:00"));     // non-digit
}

// ---------------------------------------------------------------------------
// formatEpochMs
// ---------------------------------------------------------------------------

TEST(FormatEpochMs, FixedOffsetIsExact)
{
    // 1786856207143 ms == 2026-08-16 04:56:47.143 UTC
    //                  == 2026-08-16 12:56:47.143 Beijing (+08:00)
    //                  == 2026-08-16 00:56:47.143 US Eastern (-04:00)
    const auto tz = parseDisplayTimezone("+08:00");
    ASSERT_TRUE(tz);
    EXPECT_EQ(formatEpochMs(1786856207143LL, *tz),
              "2026-08-16T12:56:47.143+08:00");

    const auto us = parseDisplayTimezone("-04:00");
    ASSERT_TRUE(us);
    EXPECT_EQ(formatEpochMs(1786856207143LL, *us),
              "2026-08-16T00:56:47.143-04:00");

    const auto utc = parseDisplayTimezone("utc");
    ASSERT_TRUE(utc);
    EXPECT_EQ(formatEpochMs(1786856207143LL, *utc),
              "2026-08-16T04:56:47.143+00:00");
}

TEST(FormatEpochMs, NegativeClamped)
{
    EXPECT_EQ(formatEpochMs(-1, DisplayTimezone::utc()),
              "1970-01-01T00:00:00.000+00:00");
}

TEST(FormatEpochMs, LocalHasShapeAndSuffix)
{
    // 1786856207143 ms == 2026-08-16 04:56:47.143 UTC.
    const auto s = formatEpochMs(1786856207143LL, DisplayTimezone::local());
    // Shape: YYYY-MM-DDTHH:MM:SS.mmm±HH:MM
    ASSERT_EQ(s.size(), 29u);
    EXPECT_EQ(s[4], '-');
    EXPECT_EQ(s[7], '-');
    EXPECT_EQ(s[10], 'T');
    EXPECT_EQ(s[13], ':');
    EXPECT_EQ(s[16], ':');
    EXPECT_EQ(s[19], '.');
    EXPECT_EQ(s[23], '+'); // machine TZ is UTC+8 (Beijing); expect '+'
    EXPECT_EQ(s[26], ':');
    EXPECT_EQ(s.substr(24, 2), "00"); // minutes part of offset
    EXPECT_EQ(s.substr(27, 2), "00");
}

TEST(FormatIsoTimestamp, NanosecondAlias)
{
    const auto tz = parseDisplayTimezone("utc");
    ASSERT_TRUE(tz);
    const Timestamp ts{
        std::chrono::system_clock::time_point{
            std::chrono::milliseconds{1786856207143LL}}};
    EXPECT_EQ(formatIsoTimestamp(ts, *tz), "2026-08-16T04:56:47.143+00:00");
}
