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
    # P2's E1 replay reads the frozen reconciliation legs.
    target_compile_definitions(test_control_engine_services
        PRIVATE
            PULSE_RECONCILIATION_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures/reconciliation"
    )
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

# P0 of the 2026-11-27 paper-team reconciliation audit: the frozen epoch contract and
# the read-only evidence fixtures. These are canary tests (they assert the committed
# fixtures, not live behaviour), so they intentionally do not link a pulse layer.
add_executable(test_reconciliation_contract
    test_reconciliation_contract.cpp
)
target_link_libraries(test_reconciliation_contract
    PRIVATE
        nlohmann_json::nlohmann_json
        GTest::gtest_main
)
target_compile_features(test_reconciliation_contract PRIVATE cxx_std_20)
target_compile_definitions(test_reconciliation_contract
    PRIVATE
        PULSE_RECONCILIATION_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures/reconciliation"
)
if(PULSE_ENABLE_SQLITE)
    target_compile_definitions(test_reconciliation_contract PRIVATE PULSE_ENABLE_SQLITE=1)
    target_link_libraries(test_reconciliation_contract PRIVATE SQLiteCpp)
endif()
include(GoogleTest)
gtest_discover_tests(test_reconciliation_contract)

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
