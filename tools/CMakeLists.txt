# Smoke test tools — manual verification scripts, not part of CTest.
# Each tool is a standalone executable for interactive testing.

add_executable(test_gate_rest
    test_gate_rest.cpp
)

target_link_libraries(test_gate_rest
    PRIVATE
        pulse::exchange
        pulse::logging
)

add_executable(test_gate_ws
    test_gate_ws.cpp
)

target_link_libraries(test_gate_ws
    PRIVATE
        pulse::exchange
        pulse::logging
)

add_executable(test_market_feed
    test_market_feed.cpp
)

target_link_libraries(test_market_feed
    PRIVATE
        pulse::market
        pulse::exchange
        pulse::logging
)

add_executable(test_execution
    test_execution.cpp
)

target_link_libraries(test_execution
    PRIVATE
        pulse::execution
        pulse::market
        pulse::exchange
        pulse::logging
)

add_executable(test_strategy
    test_strategy.cpp
)

target_link_libraries(test_strategy
    PRIVATE
        pulse::strategy
        pulse::market
        pulse::risk
        pulse::execution
        pulse::logging
)

add_executable(test_ai_pipeline
    test_ai_pipeline.cpp
)

target_link_libraries(test_ai_pipeline
    PRIVATE
        pulse::ai
        pulse::heartbeat
        pulse::strategy
        pulse::market
        pulse::logging
)

add_executable(test_balance_diag
    test_balance_diag.cpp
)

target_link_libraries(test_balance_diag
    PRIVATE
        pulse::exchange
        pulse::core
        pulse::logging
)



add_executable(test_control_smoke
    test_control_smoke.cpp
)

target_link_libraries(test_control_smoke
    PRIVATE
        pulse::control
        pulse::core
        pulse::logging
)

add_executable(backtest_data_check
    backtest_data_check.cpp
)

target_link_libraries(backtest_data_check
    PRIVATE
        pulse::backtest
        pulse::core
        pulse::exchange
        pulse::market
        pulse::logging
)

if(PULSE_ENABLE_SQLITE)
    target_link_libraries(backtest_data_check PRIVATE SQLiteCpp)
    target_compile_definitions(backtest_data_check PRIVATE PULSE_ENABLE_SQLITE)
endif()
