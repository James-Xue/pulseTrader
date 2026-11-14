# tests/unit/control — control plane unit tests
add_executable(test_control_json_rpc
    test_json_rpc_server.cpp
)
target_link_libraries(test_control_json_rpc
    PRIVATE
        pulse::control
        pulse::core
        pulse::logging
        GTest::gtest_main
)
include(GoogleTest)
gtest_discover_tests(test_control_json_rpc)

add_executable(test_control_mcp
    test_mcp_server.cpp
)
target_link_libraries(test_control_mcp
    PRIVATE
        pulse::control
        pulse::core
        pulse::logging
        GTest::gtest_main
)
include(GoogleTest)
gtest_discover_tests(test_control_mcp)

add_executable(test_control_command_parser
    test_command_parser.cpp
)
target_link_libraries(test_control_command_parser
    PRIVATE
        pulse::control
        pulse::core
        pulse::logging
        GTest::gtest_main
)
include(GoogleTest)
gtest_discover_tests(test_control_command_parser)

add_executable(test_control_idempotency
    test_order_idempotency.cpp
)
target_link_libraries(test_control_idempotency
    PRIVATE
        pulse::control
        pulse::core
        pulse::execution
        pulse::logging
        GTest::gtest_main
)
include(GoogleTest)
gtest_discover_tests(test_control_idempotency)

add_executable(test_control_order_flow
    test_order_flow_executor.cpp
)
target_link_libraries(test_control_order_flow
    PRIVATE
        pulse::control
        pulse::risk
        pulse::execution
        pulse::exchange
        pulse::core
        pulse::logging
        GTest::gtest_main
)
if(PULSE_ENABLE_SQLITE)
    target_compile_definitions(test_control_order_flow PRIVATE PULSE_ENABLE_SQLITE=1)
    target_link_libraries(test_control_order_flow PRIVATE pulse::trade_recorder)
endif()
include(GoogleTest)
gtest_discover_tests(test_control_order_flow)

add_executable(test_control_engine_services
    test_engine_services.cpp
)
target_link_libraries(test_control_engine_services
    PRIVATE
        pulse::control
        pulse::risk
        pulse::execution
        pulse::exchange
        pulse::core
        pulse::logging
        GTest::gtest_main
)
if(PULSE_ENABLE_SQLITE)
    target_compile_definitions(test_control_engine_services PRIVATE PULSE_ENABLE_SQLITE=1)
    target_link_libraries(test_control_engine_services PRIVATE pulse::trade_recorder)
endif()
include(GoogleTest)
gtest_discover_tests(test_control_engine_services)

add_executable(test_control_client
    test_control_client.cpp
)
target_link_libraries(test_control_client
    PRIVATE
        pulse::control
        pulse::core
        pulse::logging
        GTest::gtest_main
)
include(GoogleTest)
gtest_discover_tests(test_control_client)

add_executable(test_control_auth
    test_control_auth.cpp
)
target_link_libraries(test_control_auth
    PRIVATE
        pulse::control
        pulse::core
        pulse::logging
        GTest::gtest_main
)
include(GoogleTest)
gtest_discover_tests(test_control_auth)

add_executable(test_decision_intake
    test_decision_intake.cpp
)
target_link_libraries(test_decision_intake
    PRIVATE
        pulse::control
        pulse::core
        pulse::logging
        GTest::gtest_main
)
include(GoogleTest)
gtest_discover_tests(test_decision_intake)
