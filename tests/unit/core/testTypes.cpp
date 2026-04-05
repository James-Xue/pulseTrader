#include <gtest/gtest.h>

#include "pulse/core/types.hpp"
#include "pulse/core/error.hpp"
#include "pulse/core/config.hpp"

using namespace pulse;

// ---------------------------------------------------------------------------
// types.hpp
// ---------------------------------------------------------------------------

TEST(Side, OppositeIsSymmetric) {
    EXPECT_EQ(opposite(Side::Buy),  Side::Sell);
    EXPECT_EQ(opposite(Side::Sell), Side::Buy);
}

TEST(Timestamp, NowIsMonotonic) {
    const auto t1 = now();
    const auto t2 = now();
    EXPECT_LE(t1, t2);
}

TEST(Timestamp, NowHasNanosecondResolution) {
    // Verify the type carries nanoseconds (compile-time check via static_assert).
    static_assert(
        std::is_same_v<Timestamp::duration, std::chrono::nanoseconds>,
        "Timestamp must have nanosecond resolution");
}

// ---------------------------------------------------------------------------
// error.hpp
// ---------------------------------------------------------------------------

TEST(Result, OkVariant) {
    Result<int> r = 42;
    EXPECT_TRUE(ok(r));
    EXPECT_EQ(value(r), 42);
}

TEST(Result, ErrorVariant) {
    Result<int> r = PulseError{ErrorCode::NetworkTimeout, "timed out"};
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(error(r).code, ErrorCode::NetworkTimeout);
    EXPECT_EQ(error(r).message, "timed out");
}

TEST(Result, MutableValue) {
    Result<int> r = 10;
    value(r) = 20;
    EXPECT_EQ(value(r), 20);
}

// ---------------------------------------------------------------------------
// config.hpp — default values
// ---------------------------------------------------------------------------

TEST(PulseConfig, ExchangeDefaults) {
    ExchangeConfig cfg;
    EXPECT_EQ(cfg.restBaseUrl, "https://api.gateio.ws/api/v4");
    EXPECT_EQ(cfg.restTimeoutMs, 5'000u);
    EXPECT_EQ(cfg.maxRetries, 3u);
}

TEST(PulseConfig, AiDefaults) {
    AiConfig cfg;
    EXPECT_EQ(cfg.backend, "claude");
    EXPECT_EQ(cfg.heartbeatIntervalSec, 300u);
}

TEST(PulseConfig, RiskDefaults) {
    RiskConfig cfg;
    EXPECT_DOUBLE_EQ(cfg.maxDailyDrawdown, 0.02);
    EXPECT_EQ(cfg.maxOrdersPerSec, 5u);
}
