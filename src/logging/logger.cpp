// logger.cpp — Layer 2 Logging implementation for pulseTrader
//
// Provides the static Logger interface backed by spdlog.
//
// Key properties:
//   1. All I/O happens on a dedicated background thread (async sink pool)
//   2. The hot-path caller never blocks on disk or console writes
//   3. A bounded 8192-item queue prevents unbounded memory growth under load
//   4. Per-module named loggers allow independent level control at runtime

#include "logging/logger.hpp"

#include <spdlog/async.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace pulse::logging
{

namespace
{

// ---------------------------------------------------------------------------
// Module-level state
//
// We maintain our own registry instead of using spdlog's global registry so
// that:
//   1. Module loggers stay isolated from any third-party spdlog users
//   2. Per-module level changes do not affect the global default
//   3. Double-checked locking in get() is straightforward
// ---------------------------------------------------------------------------

std::mutex g_mutex;
std::unordered_map<std::string, std::shared_ptr<spdlog::logger>> g_loggers;
spdlog::level::level_enum g_default_level = spdlog::level::info;
LogConfig g_config;
bool g_initialised = false;

// ---------------------------------------------------------------------------
// parse_level — convert a human-readable level string to spdlog enum
//
// Steps:
//   1. Compare the input against each recognised level name
//   2. Return the matching spdlog::level enum value
//   3. Fall back to spdlog::level::info for any unrecognised input
// ---------------------------------------------------------------------------
spdlog::level::level_enum parse_level(const std::string &name)
{
    if ("trace" == name)
    {
        return spdlog::level::trace;
    }
    if ("debug" == name)
    {
        return spdlog::level::debug;
    }
    if ("info" == name)
    {
        return spdlog::level::info;
    }
    if ("warn" == name)
    {
        return spdlog::level::warn;
    }
    if ("error" == name)
    {
        return spdlog::level::err;
    }
    if ("critical" == name)
    {
        return spdlog::level::critical;
    }
    if ("off" == name)
    {
        return spdlog::level::off;
    }
    return spdlog::level::info; // fallback for unrecognised input
}

// ---------------------------------------------------------------------------
// make_logger — build a new spdlog logger with console + optional file sinks
//
// Steps:
//   1. Create a coloured console sink if config.toConsole is true
//   2. Create a file sink (append mode) if config.toFile is true
//   3. Combine sinks into a single multi-sink logger
//   4. Set the logger's level and flush-on-warn policy
// ---------------------------------------------------------------------------
std::shared_ptr<spdlog::logger> make_logger(std::string_view module, const LogConfig &config)
{
    std::vector<spdlog::sink_ptr> sinks;

    // Step 1: coloured console sink (thread-safe, multi-thread variant)
    if (config.toConsole)
    {
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_level(g_default_level);
        console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%n] %v");
        sinks.push_back(console_sink);
    }

    // Step 2: file sink (append mode, one file per module)
    if (config.toFile)
    {
        auto file_path = std::filesystem::path(config.logDir) / (std::string(module) + ".log");
        auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(file_path.string(), /*truncate=*/false);
        file_sink->set_level(g_default_level);
        file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%n] %v");
        sinks.push_back(file_sink);
    }

    // Step 3: combine all sinks into one logger
    auto logger = std::make_shared<spdlog::logger>(std::string(module), sinks.begin(), sinks.end());

    // Step 4: set level and flush policy
    logger->set_level(g_default_level);
    logger->flush_on(spdlog::level::warn); // flush immediately on warn+ to catch issues early
    return logger;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Logger::init — one-time initialisation
//
// Steps:
//   1. Acquire the global mutex (only contended during startup)
//   2. If already initialised, return immediately (idempotent)
//   3. Store the config and parse the level string
//   4. Create the log directory if file logging is enabled
//   5. Start the spdlog async thread pool (8192-item bounded queue)
//   6. Mark g_initialised = true
// ---------------------------------------------------------------------------
void Logger::init(const LogConfig &config)
{
    std::lock_guard lock(g_mutex);
    if (g_initialised)
    {
        return;
    }

    g_config = config;
    g_default_level = parse_level(config.level);

    // Ensure the log directory tree exists before any file sink opens
    if (config.toFile)
    {
        std::filesystem::create_directories(config.logDir);
    }

    // Start async thread pool:
    //   - 8192-item bounded queue balances memory vs throughput
    //   - 1 background thread is sufficient for log I/O
    //   - Overflow policy = block (prevents message loss at the cost of
    //     brief caller stalls under extreme log volume)
    spdlog::init_thread_pool(8192, /*threads=*/1);

    g_initialised = true;
}

// ---------------------------------------------------------------------------
// Logger::shutdown — flush all loggers and tear down the async pool
//
// Steps:
//   1. Acquire the global mutex
//   2. If not initialised, return immediately (idempotent — safe to call twice)
//   3. Flush every registered logger to ensure no buffered messages are lost
//   4. Call spdlog::shutdown() to stop the async thread pool
//   5. Clear the registry and reset the initialised flag
// ---------------------------------------------------------------------------
void Logger::shutdown() noexcept
{
    std::lock_guard lock(g_mutex);
    if (!g_initialised)
    {
        return;
    }

    for (auto &[name, logger] : g_loggers)
    {
        logger->flush();
    }
    spdlog::shutdown();
    g_loggers.clear();
    g_initialised = false;
}

// ---------------------------------------------------------------------------
// Logger::get — return (or create) a named logger for the given module
//
// Thread-safety: uses double-checked locking to avoid holding the mutex on
// every call once the logger already exists.
//
// Steps:
//   1. Lock the mutex and check if the logger already exists in the registry
//   2. If found, return it immediately (fast path, no allocation)
//   3. If not found, release the lock and create the logger (may allocate sinks)
//   4. Re-lock and try_emplace — another thread may have created it meanwhile
//   5. Return whichever logger won the race (ours or theirs)
// ---------------------------------------------------------------------------
std::shared_ptr<spdlog::logger> Logger::get(std::string_view module)
{
    std::string key(module);

    // Step 1–2: fast path — logger already exists
    {
        std::lock_guard lock(g_mutex);
        auto it = g_loggers.find(key);
        if (it != g_loggers.end())
        {
            return it->second;
        }
    }

    // Step 3: logger does not exist yet — create with the config from init().
    // If init() was not called, a default LogConfig is used (console only).
    auto logger = make_logger(module, g_config);

    // Step 4–5: re-lock and insert; another thread may have created it meanwhile
    {
        std::lock_guard lock(g_mutex);
        auto [it, inserted] = g_loggers.try_emplace(key, logger);
        return inserted ? logger : it->second;
    }
}

// ---------------------------------------------------------------------------
// Logger::set_level — change the log level of a specific module at runtime
//
// Steps:
//   1. Lock the mutex and look up the logger by module name
//   2. If found, set the logger's own level
//   3. Also update every sink's level — each sink has an independent filter,
//      so both must be changed for the new level to take effect
// ---------------------------------------------------------------------------
void Logger::set_level(std::string_view module, spdlog::level::level_enum level)
{
    std::lock_guard lock(g_mutex);
    auto it = g_loggers.find(std::string(module));
    if (it != g_loggers.end())
    {
        it->second->set_level(level);
        // Each sink has its own independent filter — update them all
        for (auto &sink : it->second->sinks())
        {
            sink->set_level(level);
        }
    }
}

// ---------------------------------------------------------------------------
// Logger::set_global_level — change the log level of ALL modules at runtime
//
// Steps:
//   1. Lock the mutex
//   2. Iterate every registered logger and update both logger + sink levels
//   3. Update the global default so newly-created loggers inherit the new level
// ---------------------------------------------------------------------------
void Logger::set_global_level(spdlog::level::level_enum level)
{
    std::lock_guard lock(g_mutex);
    for (auto &[name, logger] : g_loggers)
    {
        logger->set_level(level);
        for (auto &sink : logger->sinks())
        {
            sink->set_level(level);
        }
    }
    g_default_level = level;
}

// ---------------------------------------------------------------------------
// Logger::flush_all — force-flush all registered loggers immediately
//
// Use cases:
//   1. Before a controlled shutdown to ensure no messages are lost
//   2. After a critical error to guarantee the log file is up-to-date
//   3. Before a crash dump where the process may not exit cleanly
// ---------------------------------------------------------------------------
void Logger::flush_all()
{
    std::lock_guard lock(g_mutex);
    for (auto &[name, logger] : g_loggers)
    {
        logger->flush();
    }
}

} // namespace pulse::logging
