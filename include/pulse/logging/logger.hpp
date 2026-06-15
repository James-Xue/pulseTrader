#pragma once
// logger.hpp — Layer 2 Logging public interface for pulseTrader
//
// Provides per-module named loggers backed by spdlog's async thread pool.
//
// Key properties:
//   1. All I/O happens on a background thread — hot-path callers never block
//   2. Per-module loggers allow independent level control at runtime
//   3. A bounded 8192-item queue prevents unbounded memory growth
//   4. Thread-safe: get() uses double-checked locking; init/shutdown hold a mutex
//
// Lifecycle:
//   1. Call Logger::init(config) once at startup
//   2. Obtain per-module loggers via Logger::get("module_name")
//   3. Log messages via logger->info(...) or PULSE_LOG_INFO("module", ...) macros
//   4. Call Logger::shutdown() before process exit to flush buffered messages

#include "pulse/core/config.hpp"

#include <spdlog/spdlog.h>

#include <memory>
#include <string>
#include <string_view>

namespace pulse::logging
{

// ---------------------------------------------------------------------------
// Logger — static interface to the logging subsystem
//
// All methods are static; there is no need to instantiate this class.
// The class exists purely as a namespace with a private constructor to
// prevent accidental instantiation.
// ---------------------------------------------------------------------------
class Logger
{
  public:
    /// Initialise the global spdlog registry from PulseConfig::log.
    ///
    /// Steps:
    ///   1. Store the config and parse the level string
    ///   2. Create the log directory if it does not exist
    ///   3. Start the spdlog async thread pool (8192-item bounded queue)
    ///
    /// Must be called exactly once before any get() call.
    /// Idempotent — calling it a second time is a safe no-op.
    static void init(const LogConfig &config);

    /// Flush all sinks and shut down the async thread pool.
    ///
    /// Steps:
    ///   1. Flush every registered logger
    ///   2. Call spdlog::shutdown() to stop background threads
    ///   3. Clear the internal registry
    ///
    /// Must be called before process exit to avoid losing buffered messages.
    /// Idempotent — safe to call multiple times.
    static void shutdown() noexcept;

    /// Return (or create) a named logger for the given module.
    ///
    /// Thread-safe. Uses double-checked locking:
    ///   1. First check under lock — if found, return immediately (fast path)
    ///   2. If not found, release lock, create the logger, re-acquire lock
    ///   3. try_emplace handles the race where another thread created it meanwhile
    ///
    /// The returned shared_ptr is valid for the process lifetime.
    [[nodiscard]] static std::shared_ptr<spdlog::logger> get(std::string_view module);

    /// Change the log level of a specific module at runtime.
    ///
    /// Both the logger's own level and each sink's independent filter are updated.
    static void set_level(std::string_view module, spdlog::level::level_enum level);

    /// Change the log level of all registered modules at runtime.
    ///
    /// Also updates the global default so newly-created loggers inherit it.
    static void set_global_level(spdlog::level::level_enum level);

    /// Flush all loggers immediately.
    ///
    /// Use before a crash, controlled shutdown, or any point where you need
    /// to guarantee the log files are up-to-date.
    static void flush_all();

  private:
    Logger() = default; // prevent instantiation
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
//
// Format strings use fmt/sprintf-style placeholders: {}, {:.2f}, {:08x}, etc.
// ---------------------------------------------------------------------------

#define PULSE_LOG_TRACE(module, ...) ::pulse::logging::Logger::get(module)->trace(__VA_ARGS__)

#define PULSE_LOG_DEBUG(module, ...) ::pulse::logging::Logger::get(module)->debug(__VA_ARGS__)

#define PULSE_LOG_INFO(module, ...) ::pulse::logging::Logger::get(module)->info(__VA_ARGS__)

#define PULSE_LOG_WARN(module, ...) ::pulse::logging::Logger::get(module)->warn(__VA_ARGS__)

#define PULSE_LOG_ERROR(module, ...) ::pulse::logging::Logger::get(module)->error(__VA_ARGS__)

#define PULSE_LOG_CRITICAL(module, ...) ::pulse::logging::Logger::get(module)->critical(__VA_ARGS__)
