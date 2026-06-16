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
