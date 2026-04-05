add_executable(test_core
    testTypes.cpp
)

target_link_libraries(test_core
    PRIVATE
        pulse::core
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(test_core)
