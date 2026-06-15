add_executable(test_logging
    testLogger.cpp
)

target_link_libraries(test_logging
    PRIVATE
        pulse::logging
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(test_logging)
