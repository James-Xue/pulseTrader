add_executable(test_backtest
    test_backtest_account.cpp
    test_daily_kline_sync.cpp
    test_gate_kline_fetcher.cpp
    test_kline_loader.cpp
    test_quanto_resolver.cpp
    test_replay_driver.cpp
    test_backtest_report.cpp
    test_warmup_seeder.cpp
)

# These two suites include <SQLiteCpp/...> headers unconditionally, so they may
# only be compiled when the library they exercise exists. src/backtest applies
# the same guard to SqliteKlineReader.cpp. Without it, a PULSE_ENABLE_SQLITE=OFF
# build (the Windows default) fails in the test target.
if(PULSE_ENABLE_SQLITE)
    target_sources(test_backtest PRIVATE
        test_kline_store.cpp
        test_sqlite_kline_reader.cpp
    )
endif()

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
