#pragma once
// config_loader.hpp — Load PulseConfig from a TOML file
//
// Pipeline:
//   1. Read and parse TOML file via toml11
//   2. Resolve all "from_env:VAR_NAME" string values
//   3. Map TOML keys to PulseConfig struct fields
//   4. Caller invokes validateConfig() separately
//
// Thread-safety: safe to call from any thread; reads env vars via std::getenv.

#include "core/config.hpp"
#include "core/error.hpp"

#include <filesystem>
#include <string>

namespace pulse
{

/// Load and parse a TOML configuration file into a PulseConfig struct.
///
/// Fields absent from the TOML file retain their PulseConfig default values
/// (as declared in config.hpp). Unknown keys are silently ignored to allow
/// forward-compatible config files.
///
/// @param path  Filesystem path to the .toml file.
/// @return      Populated PulseConfig on success, PulseError on failure.
[[nodiscard]] Result<PulseConfig> loadConfigFile(const std::filesystem::path &path);

} // namespace pulse
