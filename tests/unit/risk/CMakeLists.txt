add_executable(test_risk
    test_position_manager.cpp
    test_drawdown_guard.cpp
    test_order_rate_limiter.cpp
    test_risk_manager.cpp
    test_stop_loss_engine.cpp
    test_take_profit_engine.cpp
)

target_link_libraries(test_risk
    PRIVATE
        pulse::risk
        pulse::execution
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(test_risk)
