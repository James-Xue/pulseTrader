add_executable(test_market
    test_ticker_cache.cpp
    test_symbol_registry.cpp
    test_kline_buffer.cpp
    test_orderbook_manager.cpp
)

target_link_libraries(test_market
    PRIVATE
        pulse::market
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(test_market)
