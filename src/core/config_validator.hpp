#pragma once
// config_validator.hpp — Semantic validation for PulseConfig
//
// Checks business-logic constraints that are independent of TOML syntax:
//   - Required fields non-empty (exchange credentials, symbols list)
//   - Numeric parameters within safe ranges
//   - Cross-field consistency (strategy symbols ⊆ top-level symbols)
//
// Call after load_config_file() or build_default_config() before starting
// the trading engine.

#include "core/config.hpp"
#include "core/error.hpp"

namespace pulse
{

/// Validate a fully-populated PulseConfig for semantic correctness.
///
/// @return PulseError with ConfigValidationError on first failure found,
///         or a default-constructed PulseError{Ok, ""} on success.
[[nodiscard]] PulseError validate_config(const PulseConfig &cfg);

} // namespace pulse
