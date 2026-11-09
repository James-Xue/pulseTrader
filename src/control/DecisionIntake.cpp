// DecisionIntake.cpp — validation gate for gold decision packets.
//
// Contract: gold.decision/1, documented in the decision repository
// (gold-trader-mind/15_system-boundary.md). This file owns the engine side of
// that contract; if the schema changes, both sides change together.
//
// Design rule: a decision packet is UNTRUSTED input. The brain may be wrong,
// stale, or compromised. Nothing here trusts `suggested_lots`; the risk
// arithmetic is recomputed from the stop distance and the contract size.

#include "control/DecisionIntake.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <string>

namespace pulse::control
{

namespace
{

/// XAUUSD CFD contract size: 1 lot = 100 troy ounces, so a 1.00 USD/oz move is
/// 100 USD per lot. Fixed here because the venue is fixed (Gate TradFi).
constexpr double kOuncesPerLot = 100.0;

/// Largest risk a single packet may request, in units of the account's
/// single-trade risk budget. Mirrors 05_risk-rules.md (1R per trade).
constexpr double kMaxRiskR = 1.0;

/// Current wall-clock time in epoch seconds.
std::int64_t nowEpochSeconds()
{
    return static_cast<std::int64_t>(std::time(nullptr));
}

/// Require a numeric field and return it, or report which one is missing.
bool readNumber(const nlohmann::json &object, const char *key, double &out)
{
    if (!object.contains(key) || !object[key].is_number())
    {
        return false;
    }
    out = object[key].get<double>();
    return true;
}

} // namespace

bool parseIso8601Utc(const std::string &text, std::int64_t &epochSeconds)
{
    // The contract uses one fixed shape, so a strict parse is enough and avoids
    // pulling in a date library: YYYY-MM-DDTHH:MM:SSZ, exactly 20 characters.
    if (20 != text.size() || 'T' != text[10] || 'Z' != text[19])
    {
        return false;
    }

    std::tm tm{};
    tm.tm_year = std::stoi(text.substr(0, 4)) - 1900;
    tm.tm_mon = std::stoi(text.substr(5, 2)) - 1;
    tm.tm_mday = std::stoi(text.substr(8, 2));
    tm.tm_hour = std::stoi(text.substr(11, 2));
    tm.tm_min = std::stoi(text.substr(14, 2));
    tm.tm_sec = std::stoi(text.substr(17, 2));

    // All values are validated by round-tripping: a field out of range would
    // otherwise normalise silently (month 13 becomes January of the next year).
    if (tm.tm_mon < 0 || tm.tm_mon > 11
        || tm.tm_mday < 1 || tm.tm_mday > 31
        || tm.tm_hour < 0 || tm.tm_hour > 23
        || tm.tm_min < 0 || tm.tm_min > 59
        || tm.tm_sec < 0 || tm.tm_sec > 60)
    {
        return false;
    }

    epochSeconds = static_cast<std::int64_t>(::timegm(&tm));
    return true;
}

Result<nlohmann::json> validateDecision(const nlohmann::json &packet)
{
    // 1. Shape.
    if (!packet.is_object())
    {
        return PulseError{ ErrorCode::ControlInvalidRequest,
                           "decision packet must be a JSON object" };
    }
    if (!packet.contains("schema") || !packet["schema"].is_string()
        || "gold.decision/1" != packet["schema"].get<std::string>())
    {
        return PulseError{ ErrorCode::ControlInvalidRequest,
                           "decision.schema must be \"gold.decision/1\"" };
    }

    // 2. Identity and freshness. A stale packet is discarded, never "caught
    //    up": the levels were computed for a market state that is gone.
    if (!packet.contains("id") || !packet["id"].is_string()
        || packet["id"].get<std::string>().empty())
    {
        return PulseError{ ErrorCode::ControlInvalidRequest,
                           "decision.id must be a non-empty string" };
    }
    const std::string id = packet["id"].get<std::string>();

    if (!packet.contains("valid_until") || !packet["valid_until"].is_string())
    {
        return PulseError{ ErrorCode::ControlInvalidRequest,
                           "decision.valid_until must be an ISO-8601 UTC string" };
    }
    std::int64_t validUntil = 0;
    if (!parseIso8601Utc(packet["valid_until"].get<std::string>(), validUntil))
    {
        return PulseError{ ErrorCode::ControlInvalidRequest,
                           "decision.valid_until is not YYYY-MM-DDTHH:MM:SSZ" };
    }

    nlohmann::json verdict;
    verdict["id"] = id;
    verdict["dry_run"] = true;
    verdict["would_execute"] = false;
    verdict["warnings"] = nlohmann::json::array();

    if (validUntil < nowEpochSeconds())
    {
        verdict["accepted"] = false;
        verdict["reason"] = "expired";
        return verdict;
    }

    // 3. Instrument. The venue is fixed by the 2026-11-09 decision: Gate
    //    TradFi CFD only. Anything else is a contract mismatch, not a signal.
    if (!packet.contains("instrument") || !packet["instrument"].is_object())
    {
        return PulseError{ ErrorCode::ControlInvalidRequest,
                           "decision.instrument must be an object" };
    }
    const auto &instrument = packet["instrument"];
    if (!instrument.contains("venue") || !instrument["venue"].is_string()
        || "gate-tradfi" != instrument["venue"].get<std::string>())
    {
        return PulseError{ ErrorCode::ControlInvalidRequest,
                           "decision.instrument.venue must be \"gate-tradfi\"" };
    }
    if (!instrument.contains("contract") || !instrument["contract"].is_string()
        || "cfd" != instrument["contract"].get<std::string>())
    {
        return PulseError{ ErrorCode::ControlInvalidRequest,
                           "decision.instrument.contract must be \"cfd\"" };
    }
    if (!instrument.contains("symbol") || !instrument["symbol"].is_string()
        || instrument["symbol"].get<std::string>().empty())
    {
        return PulseError{ ErrorCode::ControlInvalidRequest,
                           "decision.instrument.symbol must be non-empty" };
    }

    // 4. Stance. "abstain" is a successful validation that produces no action.
    if (!packet.contains("stance") || !packet["stance"].is_string())
    {
        return PulseError{ ErrorCode::ControlInvalidRequest,
                           "decision.stance must be a string" };
    }
    const std::string stance = packet["stance"].get<std::string>();
    static const std::array<const char *, 4> kStances{ "long", "short", "flat",
                                                       "abstain" };
    bool stanceKnown = false;
    for (const char *candidate : kStances)
    {
        if (stance == candidate)
        {
            stanceKnown = true;
            break;
        }
    }
    if (!stanceKnown)
    {
        return PulseError{ ErrorCode::ControlInvalidRequest,
                           "decision.stance must be long|short|flat|abstain" };
    }
    if ("abstain" == stance)
    {
        verdict["accepted"] = false;
        verdict["reason"] = "abstain";
        return verdict;
    }

    // 5. Level geometry. Longs put the stop below the entry zone, shorts above;
    //    a packet that violates this is not "unusual", it is inverted.
    if (!packet.contains("levels") || !packet["levels"].is_object())
    {
        return PulseError{ ErrorCode::ControlInvalidRequest,
                           "decision.levels must be an object" };
    }
    const auto &levels = packet["levels"];

    if (!levels.contains("entry_zone") || !levels["entry_zone"].is_array()
        || 2 != levels["entry_zone"].size())
    {
        return PulseError{ ErrorCode::ControlInvalidRequest,
                           "decision.levels.entry_zone must be [low, high]" };
    }
    const double entryLow = levels["entry_zone"][0].get<double>();
    const double entryHigh = levels["entry_zone"][1].get<double>();
    if (entryLow <= 0.0 || entryHigh < entryLow)
    {
        return PulseError{ ErrorCode::ControlInvalidRequest,
                           "decision.levels.entry_zone must satisfy 0 < low <= high" };
    }

    double stop = 0.0;
    if (!readNumber(levels, "stop", stop) || stop <= 0.0)
    {
        return PulseError{ ErrorCode::ControlInvalidRequest,
                           "decision.levels.stop must be a positive number" };
    }
    if ("long" == stance && stop >= entryLow)
    {
        return PulseError{ ErrorCode::ControlInvalidRequest,
                           "long stop must sit below the entry zone" };
    }
    if ("short" == stance && stop <= entryHigh)
    {
        return PulseError{ ErrorCode::ControlInvalidRequest,
                           "short stop must sit above the entry zone" };
    }

    if (!levels.contains("targets") || !levels["targets"].is_array()
        || levels["targets"].empty())
    {
        return PulseError{ ErrorCode::ControlInvalidRequest,
                           "decision.levels.targets must be a non-empty array" };
    }

    // 6. Sizing. Only the stop distance is trusted; the lot count is recomputed.
    if (!packet.contains("sizing") || !packet["sizing"].is_object())
    {
        return PulseError{ ErrorCode::ControlInvalidRequest,
                           "decision.sizing must be an object" };
    }
    const auto &sizing = packet["sizing"];

    double riskR = 0.0;
    if (!readNumber(sizing, "risk_R", riskR) || riskR <= 0.0)
    {
        return PulseError{ ErrorCode::ControlInvalidRequest,
                           "decision.sizing.risk_R must be positive" };
    }
    if (riskR > kMaxRiskR)
    {
        return PulseError{ ErrorCode::ControlInvalidRequest,
                           "decision.sizing.risk_R exceeds the 1R limit" };
    }

    double stopDistance = 0.0;
    if (!readNumber(sizing, "stop_distance", stopDistance)
        || stopDistance <= 0.0)
    {
        return PulseError{ ErrorCode::ControlInvalidRequest,
                           "decision.sizing.stop_distance must be positive" };
    }

    // Cross-check: the declared stop distance must match the levels, otherwise
    // the two halves of the packet came from different market states.
    const double impliedDistance =
        ("long" == stance) ? (entryLow - stop) : (stop - entryHigh);
    const double tolerance = 0.01 * std::max(1.0, impliedDistance);
    if (std::abs(impliedDistance - stopDistance) > tolerance)
    {
        verdict["warnings"].push_back(
            "stop_distance disagrees with levels by more than 1%");
    }

    // 7. Recomputed arithmetic. The engine owns this number, not the packet.
    const double riskPerLotUsd = stopDistance * kOuncesPerLot;

    nlohmann::json computed;
    computed["risk_per_lot_usd"] = riskPerLotUsd;
    computed["ounces_per_lot"] = kOuncesPerLot;
    computed["risk_R"] = riskR;
    computed["entry_reference"] =
        ("long" == stance) ? entryHigh : entryLow;
    computed["note"] =
        "lots are NOT trusted; a live path must derive them from account equity";

    verdict["accepted"] = true;
    verdict["dry_run"] = true;
    verdict["would_execute"] = false;
    verdict["stance"] = stance;
    verdict["computed"] = computed;
    verdict["reason"] =
        "validated; dry run only — no order was placed";
    return verdict;
}

} // namespace pulse::control
