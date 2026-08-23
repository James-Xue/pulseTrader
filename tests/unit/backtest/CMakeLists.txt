add_executable(test_backtest
    test_gate_kline_fetcher.cpp
    test_kline_loader.cpp
    test_sqlite_kline_reader.cpp
)

target_link_libraries(test_backtest
    PRIVATE
        pulse::backtest
        pulse::core
        pulse::exchange
        pulse::market
        GTest::gtest_main
)

if(PULSE_ENABLE_SQLITE)
    target_link_libraries(test_backtest PRIVATE pulse::backtest SQLiteCpp)
endif()

include(GoogleTest)
gtest_discover_tests(test_backtest)
