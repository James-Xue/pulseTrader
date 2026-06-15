#include "pulse/logging/logger.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace pulse::logging
{
namespace
{

class LoggerTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        // Use a unique temp directory for each test.
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

TEST_F(LoggerTest, InitCreatesLogDirectory)
{
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
    LogConfig config;
    config.level = "info";
    config.logDir = test_log_dir_.string();
    config.toConsole = false;
    config.toFile = true;
    Logger::init(config);

    auto log = Logger::get("dynamic_level");

    // Initially debug messages should be filtered.
    log->debug("filtered_debug");
    log->flush();

    // Change to debug level.
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
    LogConfig config;
    config.logDir = test_log_dir_.string();
    config.toConsole = false;
    config.toFile = true;
    Logger::init(config);

    Logger::shutdown();
    // Second shutdown should not crash.
    Logger::shutdown();
    SUCCEED();
}

} // namespace
} // namespace pulse::logging
