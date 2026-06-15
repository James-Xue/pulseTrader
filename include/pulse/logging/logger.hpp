#pragma once

#include "pulse/core/config.hpp"

#include <spdlog/spdlog.h>

#include <memory>
#include <string>
#include <string_view>

namespace pulse::logging {

// ---------------------------------------------------------------------------
// Logger
//
// Thin wrapper around spdlog providing per-module named loggers with an
// asynchronous sink and bounded queue. All I/O happens on a background
// thread so callers on the hot path never block.
//
// Usage:
//   logging::Logger::init(config);              // once at startup
//   auto& log = logging::Logger::get("market"); // per-module logger
//   log->info("orderbook depth: {}", depth);
//   logging::Logger::shutdown();                // before exit
//
// Or use the convenience macros:
//   PULSE_LOG_INFO("market", "orderbook depth: {}", depth);
// ---------------------------------------------------------------------------

class Logger {
public:
    /// Initialise the global spdlog registry from PulseConfig::log.
    /// Creates the log directory if it does not exist.
    /// Must be called exactly once before any get() call.
    static void init(const LogConfig& config);

    /// Flush all sinks and shut down the async thread pool.
    /// Must be called before process exit to avoid losing buffered messages.
    static void shutdown() noexcept;

    /// Return (or create) a named logger for the given module.
    /// Thread-safe. The returned shared_ptr is valid for the process lifetime.
    [[nodiscard]] static std::shared_ptr<spdlog::logger> get(std::string_view module);

    /// Change the log level of a specific module at runtime.
    static void set_level(std::string_view module, spdlog::level::level_enum level);

    /// Change the log level of all registered modules at runtime.
    static void set_global_level(spdlog::level::level_enum level);

    /// Flush all loggers immediately (useful before a crash or shutdown).
    static void flush_all();

private:
    Logger() = default;
};

} // namespace pulse::logging

// ---------------------------------------------------------------------------
// Convenience macros
//
// Usage:
//   PULSE_LOG_INFO("exchange", "connected to {}", url);
//   PULSE_LOG_WARN("risk",     "drawdown {:.2%} approaching limit", dd);
//   PULSE_LOG_ERROR("ai",      "LLM response failed validation");
//
// The module string is used as the spdlog logger name. If the logger has
// not been created yet, it is created on first use (with the default level
// set during init()).
// ---------------------------------------------------------------------------

#define PULSE_LOG_TRACE(module, ...) \
    ::pulse::logging::Logger::get(module)->trace(__VA_ARGS__)

#define PULSE_LOG_DEBUG(module, ...) \
    ::pulse::logging::Logger::get(module)->debug(__VA_ARGS__)

#define PULSE_LOG_INFO(module, ...) \
    ::pulse::logging::Logger::get(module)->info(__VA_ARGS__)

#define PULSE_LOG_WARN(module, ...) \
    ::pulse::logging::Logger::get(module)->warn(__VA_ARGS__)

#define PULSE_LOG_ERROR(module, ...) \
    ::pulse::logging::Logger::get(module)->error(__VA_ARGS__)

#define PULSE_LOG_CRITICAL(module, ...) \
    ::pulse::logging::Logger::get(module)->critical(__VA_ARGS__)
