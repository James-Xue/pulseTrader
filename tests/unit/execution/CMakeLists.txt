add_executable(test_execution_unit
    test_execution_report.cpp
    test_order_executor.cpp
    test_order_tracker.cpp
)

target_link_libraries(test_execution_unit
    PRIVATE
        pulse::execution
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(test_execution_unit)
