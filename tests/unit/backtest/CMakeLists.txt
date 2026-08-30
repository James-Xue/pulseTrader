# test_sqlite_kline_reader.cpp needs SqliteKlineReader, which is compiled into
# pulse_backtest only when PULSE_ENABLE_SQLITE=ON — keep the test list in sync.
set(BACKTEST_TEST_SOURCES
    test_backtest_account.cpp
    test_daily_kline_sync.cpp
    test_gate_kline_fetcher.cpp
    test_kline_loader.cpp
    test_replay_driver.cpp
    test_backtest_report.cpp
    test_warmup_seeder.cpp
)
if(PULSE_ENABLE_SQLITE)
    list(APPEND BACKTEST_TEST_SOURCES test_sqlite_kline_reader.cpp)
endif()

add_executable(test_backtest ${BACKTEST_TEST_SOURCES})

target_link_libraries(test_backtest
    PRIVATE
        pulse::backtest
        pulse::core
        pulse::exchange
        pulse::market
        GTest::gtest_main
)

if(PULSE_ENABLE_SQLITE)
    target_link_libraries(test_backtest PRIVATE SQLiteCpp)
endif()

include(GoogleTest)
gtest_discover_tests(test_backtest)
