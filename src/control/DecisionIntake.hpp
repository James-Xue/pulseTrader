#pragma once

#include "core/PulseError.hpp"

#include <nlohmann/json.hpp>

namespace pulse::control
{

/// Validate a gold decision packet and return a dry-run verdict.
///
/// The packet is the single interface between the decision repository
/// (gold-trader-mind) and this execution engine. This function is the gate:
/// it checks the contract, the instrument, the level geometry and the risk
/// arithmetic, and reports what WOULD happen.
///
/// It never places, cancels or modifies an order. Live execution is a separate
/// step that must be added deliberately, with its own tests and its own risk
/// checks — see docs/decision-intake.md.
///
/// Errors (ControlInvalidRequest) are for malformed or unsafe packets.
/// A well-formed packet that simply says "do nothing" (`stance == "abstain"`)
/// is a successful validation with accepted=false, not an error.
[[nodiscard]] Result<nlohmann::json>
validateDecision(const nlohmann::json &packet);

/// Parse an ISO-8601 UTC timestamp ("YYYY-MM-DDTHH:MM:SSZ").
/// Returns false when the string is malformed.
[[nodiscard]] bool parseIso8601Utc(const std::string &text,
                                   std::int64_t &epochSeconds);

} // namespace pulse::control
