// testLogger.cpp — Unit tests for pulse/logging/logger.hpp
//
// Test coverage:
//   1. Init creates the log directory tree
//   2. get() returns the same logger for the same module name (singleton)
//   3. get() returns different loggers for different module names
//   4. Log messages are written to the correct per-module file
//   5. Log level filtering works at init time
//   6. set_level() changes the filter at runtime
//   7. PULSE_LOG_* macros compile and produce output
//   8. shutdown() is idempotent (safe to call twice)

#include "pulse/logging/logger.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace pulse::logging
{
namespace
{

// ---------------------------------------------------------------------------
// Test fixture — creates a unique temp directory per test, cleans up after
// ---------------------------------------------------------------------------
class LoggerTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        // Each test gets its own temp directory to avoid cross-test interference
        test_log_dir_ = std::filesystem::temp_directory_path() / ("pulse_log_test_" + std::to_string(getpid()));
        std::filesystem::create_directories(test_log_dir_);
    }

    void TearDown() override
    {
        Logger::shutdown();
        std::filesystem::remove_all(test_log_dir_);
    }

    std::filesystem::path test_log_dir_;
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_F(LoggerTest, InitCreatesLogDirectory)
{
    // init() must create the log directory (and parents) if it does not exist
    auto dir = test_log_dir_ / "subdir";
    LogConfig config;
    config.logDir = dir.string();
    config.toConsole = false;
    config.toFile = true;

    Logger::init(config);
    EXPECT_TRUE(std::filesystem::exists(dir));
}

TEST_F(LoggerTest, GetReturnsSameLoggerForSameModule)
{
    // get("market") called twice must return the same shared_ptr
    LogConfig config;
    config.logDir = test_log_dir_.string();
    config.toConsole = false;
    config.toFile = true;
    Logger::init(config);

    auto log1 = Logger::get("market");
    auto log2 = Logger::get("market");
    EXPECT_EQ(log1.get(), log2.get());
}

TEST_F(LoggerTest, GetReturnsDifferentLoggersForDifferentModules)
{
    // get("market") and get("risk") must return different loggers
    LogConfig config;
    config.logDir = test_log_dir_.string();
    config.toConsole = false;
    config.toFile = true;
    Logger::init(config);

    auto log_market = Logger::get("market");
    auto log_risk = Logger::get("risk");
    EXPECT_NE(log_market.get(), log_risk.get());
}

TEST_F(LoggerTest, LoggerWritesToFile)
{
    // A log message must appear in the corresponding module's .log file
    LogConfig config;
    config.logDir = test_log_dir_.string();
    config.toConsole = false;
    config.toFile = true;
    Logger::init(config);

    auto log = Logger::get("test_module");
    log->info("hello from test");
    log->flush();

    auto log_file = test_log_dir_ / "test_module.log";
    EXPECT_TRUE(std::filesystem::exists(log_file));

    std::ifstream ifs(log_file);
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("hello from test"), std::string::npos);
}

TEST_F(LoggerTest, LogLevelFiltersMessages)
{
    // With level="warn", info messages must be filtered; warn must pass through
    LogConfig config;
    config.level = "warn"; // only warn+ should be written
    config.logDir = test_log_dir_.string();
    config.toConsole = false;
    config.toFile = true;
    Logger::init(config);

    auto log = Logger::get("level_test");
    log->info("should not appear");
    log->warn("should appear");
    log->flush();

    auto log_file = test_log_dir_ / "level_test.log";
    std::ifstream ifs(log_file);
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content.find("should not appear"), std::string::npos);
    EXPECT_NE(content.find("should appear"), std::string::npos);
}

TEST_F(LoggerTest, SetLevelChangesFilterAtRuntime)
{
    // set_level() must change the filter without requiring re-initialisation
    LogConfig config;
    config.level = "info";
    config.logDir = test_log_dir_.string();
    config.toConsole = false;
    config.toFile = true;
    Logger::init(config);

    auto log = Logger::get("dynamic_level");

    // Initially, debug messages should be filtered (level = info)
    log->debug("filtered_debug");
    log->flush();

    // After changing to debug level, debug messages should appear
    Logger::set_level("dynamic_level", spdlog::level::debug);
    log->debug("visible_debug");
    log->flush();

    auto log_file = test_log_dir_ / "dynamic_level.log";
    std::ifstream ifs(log_file);
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content.find("filtered_debug"), std::string::npos);
    EXPECT_NE(content.find("visible_debug"), std::string::npos);
}

TEST_F(LoggerTest, MacrosCompileAndLog)
{
    // PULSE_LOG_INFO and PULSE_LOG_WARN macros must compile and produce output
    LogConfig config;
    config.logDir = test_log_dir_.string();
    config.toConsole = false;
    config.toFile = true;
    Logger::init(config);

    PULSE_LOG_INFO("macro_test", "value={}", 42);
    PULSE_LOG_WARN("macro_test", "warning!");
    Logger::get("macro_test")->flush();

    auto log_file = test_log_dir_ / "macro_test.log";
    std::ifstream ifs(log_file);
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("value=42"), std::string::npos);
    EXPECT_NE(content.find("warning!"), std::string::npos);
}

TEST_F(LoggerTest, ShutdownIsIdempotent)
{
    // Calling shutdown() twice must not crash or throw
    LogConfig config;
    config.logDir = test_log_dir_.string();
    config.toConsole = false;
    config.toFile = true;
    Logger::init(config);

    Logger::shutdown();
    // Second shutdown must be a safe no-op
    Logger::shutdown();
    SUCCEED();
}

} // namespace
} // namespace pulse::logging
