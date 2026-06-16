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
