add_executable(test_funding
    test_funding_watch.cpp
)

target_link_libraries(test_funding
    PRIVATE
        pulse::funding
        pulse::strategy
        pulse::exchange
        pulse::market
        pulse::logging
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(test_funding)
