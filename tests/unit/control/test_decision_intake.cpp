// test_decision_intake.cpp — validation gate for gold decision packets
//
// The intake is a pure function over untrusted JSON, so these tests need no
// socket, no exchange and no clock beyond a far-future expiry.

#include "control/DecisionIntake.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace pulse;
using namespace pulse::control;

namespace
{

/// A minimal valid packet. Individual tests mutate one field at a time so a
/// failure names the rule that broke.
nlohmann::json validPacket()
{
    return nlohmann::json{
        { "schema", "gold.decision/1" },
        { "id", "2026-11-09T14:05:00Z-xauusd-01" },
        { "created_at", "2026-11-09T14:05:00Z" },
        { "valid_until", "2099-01-01T00:00:00Z" },
        { "instrument", { { "symbol", "XAUUSD" },
                          { "venue", "gate-tradfi" },
                          { "contract", "cfd" } } },
        { "stance", "long" },
        { "playbook", "B" },
        { "conviction", 0.62 },
        { "levels", { { "entry_zone", { 4021.5, 4025.0 } },
                      { "stop", 4012.0 },
                      { "targets", { 4038.0, 4052.0 } } } },
        { "sizing", { { "risk_R", 1.0 },
                      { "suggested_lots", 0.01 },
                      { "stop_distance", 8.5 } } },
    };
}

} // anonymous namespace

TEST(DecisionIntake, ValidLongPacketIsAcceptedAsDryRun)
{
    const auto result = validateDecision(validPacket());
    ASSERT_TRUE(ok(result)) << error(result).message;

    const nlohmann::json &verdict = value(result);
    EXPECT_TRUE(verdict["accepted"].get<bool>());
    EXPECT_TRUE(verdict["dry_run"].get<bool>());
    // The whole point of this milestone: validation must never trade.
    EXPECT_FALSE(verdict["would_execute"].get<bool>());

    // 8.5 USD/oz * 100 oz = 850 USD risk per lot.
    const double riskPerLot = verdict["computed"]["risk_per_lot_usd"].get<double>();
    EXPECT_DOUBLE_EQ(850.0, riskPerLot);
}

TEST(DecisionIntake, AbstainIsAValidPacketThatDoesNothing)
{
    auto packet = validPacket();
    packet["stance"] = "abstain";

    const auto result = validateDecision(packet);
    ASSERT_TRUE(ok(result)) << error(result).message;
    EXPECT_FALSE(value(result)["accepted"].get<bool>());
    EXPECT_EQ("abstain", value(result)["reason"].get<std::string>());
}

TEST(DecisionIntake, ExpiredPacketIsRejected)
{
    auto packet = validPacket();
    packet["valid_until"] = "2020-01-01T00:00:00Z";

    const auto result = validateDecision(packet);
    ASSERT_TRUE(ok(result)) << error(result).message;
    EXPECT_FALSE(value(result)["accepted"].get<bool>());
    EXPECT_EQ("expired", value(result)["reason"].get<std::string>());
}

TEST(DecisionIntake, WrongSchemaIsAnError)
{
    auto packet = validPacket();
    packet["schema"] = "gold.decision/2";

    const auto result = validateDecision(packet);
    ASSERT_FALSE(ok(result));
    EXPECT_EQ(ErrorCode::ControlInvalidRequest, error(result).code);
}

TEST(DecisionIntake, NonGateVenueIsRejected)
{
    auto packet = validPacket();
    packet["instrument"]["venue"] = "comex";

    const auto result = validateDecision(packet);
    ASSERT_FALSE(ok(result));
    EXPECT_EQ(ErrorCode::ControlInvalidRequest, error(result).code);
}

TEST(DecisionIntake, LongStopAboveEntryIsRejected)
{
    auto packet = validPacket();
    packet["levels"]["stop"] = 4030.0;

    const auto result = validateDecision(packet);
    ASSERT_FALSE(ok(result));
    EXPECT_EQ(ErrorCode::ControlInvalidRequest, error(result).code);
}

TEST(DecisionIntake, RiskAboveOneRIsRejected)
{
    auto packet = validPacket();
    packet["sizing"]["risk_R"] = 2.0;

    const auto result = validateDecision(packet);
    ASSERT_FALSE(ok(result));
    EXPECT_EQ(ErrorCode::ControlInvalidRequest, error(result).code);
}

TEST(DecisionIntake, StopDistanceMismatchProducesAWarning)
{
    auto packet = validPacket();
    // Levels imply 9.5, the packet claims 8.5.
    packet["sizing"]["stop_distance"] = 8.5;
    packet["levels"]["stop"] = 4011.5;

    const auto result = validateDecision(packet);
    ASSERT_TRUE(ok(result)) << error(result).message;
    const nlohmann::json &verdict = value(result);
    EXPECT_TRUE(verdict["accepted"].get<bool>());
    ASSERT_TRUE(verdict["warnings"].is_array());
    EXPECT_FALSE(verdict["warnings"].empty());
}

TEST(DecisionIntake, ShortPacketWithStopBelowEntryIsRejected)
{
    auto packet = validPacket();
    packet["stance"] = "short";
    packet["levels"]["entry_zone"] = { 4021.5, 4025.0 };
    packet["levels"]["stop"] = 4020.0; // below the zone: inverted

    const auto result = validateDecision(packet);
    ASSERT_FALSE(ok(result));
    EXPECT_EQ(ErrorCode::ControlInvalidRequest, error(result).code);
}

TEST(DecisionIntake, Iso8601ParserAcceptsAndRejects)
{
    std::int64_t epoch = 0;
    EXPECT_TRUE(parseIso8601Utc("2026-11-09T14:05:00Z", epoch));
    EXPECT_GT(epoch, 0);

    EXPECT_FALSE(parseIso8601Utc("2026-11-09 14:05:00", epoch));
    EXPECT_FALSE(parseIso8601Utc("2026-13-09T14:05:00Z", epoch));
    EXPECT_FALSE(parseIso8601Utc("", epoch));
}
