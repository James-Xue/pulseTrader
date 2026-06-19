# tests/unit/trade_recorder/CMakeLists.txt

add_executable(test_trade_recorder_unit
    test_trade_recorder.cpp
    test_trade_queries.cpp
)

target_link_libraries(test_trade_recorder_unit
    PRIVATE
        pulse::trade_recorder
        pulse::execution
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(test_trade_recorder_unit)
