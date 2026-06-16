add_executable(test_exchange
    test_gate_auth.cpp
    test_gate_ws_channels.cpp
    test_gate_ws_client.cpp
)

target_link_libraries(test_exchange
    PRIVATE
        pulse::exchange
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(test_exchange)
