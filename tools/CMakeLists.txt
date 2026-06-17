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

if(PULSE_ENABLE_WEBUI)
    add_executable(test_webui_server
        test_webui.cpp
    )

    target_include_directories(test_webui_server
        PRIVATE
            ${PROJECT_SOURCE_DIR}/third_party/uWebSockets
            ${PROJECT_SOURCE_DIR}/third_party/uSockets
    )

    target_link_libraries(test_webui_server
        PRIVATE
            pulse::webui
            pulse::market
            pulse::strategy
            pulse::risk
            pulse::execution
            pulse::ai
            pulse::exchange
            pulse::logging
            ${PROJECT_SOURCE_DIR}/third_party/uSockets/uSockets.a
            z
    )
endif()
