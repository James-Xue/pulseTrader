add_executable(test_webui
    test_snapshot_types.cpp
    test_dashboard_state.cpp
    test_web_server.cpp
    test_ws_server.cpp
)

target_include_directories(test_webui
    PRIVATE
        # uWebSockets headers needed for PerSocketData and OpCode in tests
        ${PROJECT_SOURCE_DIR}/third_party/uWebSockets
        ${PROJECT_SOURCE_DIR}/third_party/uSockets
)

target_link_libraries(test_webui
    PRIVATE
        pulse::webui
        pulse::market
        pulse::strategy
        pulse::risk
        pulse::execution
        pulse::ai
        # uSockets static library (required by web_server.cpp via pulse_webui)
        ${PROJECT_SOURCE_DIR}/third_party/uSockets/uSockets.a
        z
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(test_webui)
