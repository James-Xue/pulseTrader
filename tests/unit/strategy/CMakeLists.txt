add_executable(test_strategy_unit
    test_signal_types.cpp
    test_strategy_params.cpp
    test_strategy_base.cpp
    test_strategy_manager.cpp
    test_momentum_scalper.cpp
    test_orderbook_scalper.cpp
    test_mean_reversion_scalper.cpp
    test_supertrend_scalper.cpp
    test_unified_scalper.cpp
    test_strategy_registry.cpp
    test_signal_aggregator.cpp
    test_signal_board.cpp
)

target_link_libraries(test_strategy_unit
    PRIVATE
        pulse::strategy
        pulse::market
        pulse::risk
        pulse::execution
        pulse::logging
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(test_strategy_unit)
