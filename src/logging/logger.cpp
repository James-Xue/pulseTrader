#include "pulse/logging/logger.hpp"

#include <spdlog/async.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace pulse::logging {

namespace {

// Thread-safe registry of named loggers.
// spdlog's global registry is avoided to keep module loggers isolated and
// to allow per-module level changes without affecting the global default.
std::mutex                                      g_mutex;
std::unordered_map<std::string, std::shared_ptr<spdlog::logger>> g_loggers;
spdlog::level::level_enum                       g_default_level = spdlog::level::info;
LogConfig                                       g_config;
bool                                            g_initialised   = false;

// Convert a string level name ("trace", "debug", ...) to spdlog enum.
spdlog::level::level_enum parse_level(const std::string& name) {
    if (name == "trace")    return spdlog::level::trace;
    if (name == "debug")    return spdlog::level::debug;
    if (name == "info")     return spdlog::level::info;
    if (name == "warn")     return spdlog::level::warn;
    if (name == "error")    return spdlog::level::err;
    if (name == "critical") return spdlog::level::critical;
    if (name == "off")      return spdlog::level::off;
    return spdlog::level::info;  // fallback
}

// Build a logger with console + optional file sinks.
std::shared_ptr<spdlog::logger> make_logger(
    std::string_view module,
    const LogConfig& config)
{
    std::vector<spdlog::sink_ptr> sinks;

    if (config.toConsole) {
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_level(g_default_level);
        console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%n] %v");
        sinks.push_back(console_sink);
    }

    if (config.toFile) {
        auto file_path = std::filesystem::path(config.logDir)
                       / (std::string(module) + ".log");
        auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
            file_path.string(), /*truncate=*/false);
        file_sink->set_level(g_default_level);
        file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%n] %v");
        sinks.push_back(file_sink);
    }

    auto logger = std::make_shared<spdlog::logger>(std::string(module), sinks.begin(), sinks.end());
    logger->set_level(g_default_level);
    logger->flush_on(spdlog::level::warn);  // flush immediately on warn+
    return logger;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Logger static interface
// ---------------------------------------------------------------------------

void Logger::init(const LogConfig& config) {
    std::lock_guard lock(g_mutex);
    if (g_initialised) return;

    g_config        = config;
    g_default_level = parse_level(config.level);

    // Ensure log directory exists.
    if (config.toFile) {
        std::filesystem::create_directories(config.logDir);
    }

    // Initialise spdlog async thread pool.
    // 8192-item bounded queue; overflow policy = block (prevents message loss
    // at the cost of brief caller stalls under extreme log volume).
    spdlog::init_thread_pool(8192, /*threads=*/1);

    g_initialised = true;
}

void Logger::shutdown() noexcept {
    std::lock_guard lock(g_mutex);
    if (!g_initialised) return;

    for (auto& [name, logger] : g_loggers) {
        logger->flush();
    }
    spdlog::shutdown();
    g_loggers.clear();
    g_initialised = false;
}

std::shared_ptr<spdlog::logger> Logger::get(std::string_view module) {
    std::string key(module);
    {
        std::lock_guard lock(g_mutex);
        auto it = g_loggers.find(key);
        if (it != g_loggers.end()) {
            return it->second;
        }
    }

    // Logger does not exist yet — create with the config stored during init().
    // If init() was not called, a default LogConfig is used (console only).
    auto logger = make_logger(module, g_config);

    {
        std::lock_guard lock(g_mutex);
        // Double-check: another thread may have created it while we were
        // building ours.
        auto [it, inserted] = g_loggers.try_emplace(key, logger);
        return inserted ? logger : it->second;
    }
}

void Logger::set_level(std::string_view module, spdlog::level::level_enum level) {
    std::lock_guard lock(g_mutex);
    auto it = g_loggers.find(std::string(module));
    if (it != g_loggers.end()) {
        it->second->set_level(level);
        // Also update all sinks — each sink has its own independent filter.
        for (auto& sink : it->second->sinks()) {
            sink->set_level(level);
        }
    }
}

void Logger::set_global_level(spdlog::level::level_enum level) {
    std::lock_guard lock(g_mutex);
    for (auto& [name, logger] : g_loggers) {
        logger->set_level(level);
        for (auto& sink : logger->sinks()) {
            sink->set_level(level);
        }
    }
    g_default_level = level;
}

void Logger::flush_all() {
    std::lock_guard lock(g_mutex);
    for (auto& [name, logger] : g_loggers) {
        logger->flush();
    }
}

} // namespace pulse::logging
