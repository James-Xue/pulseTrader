add_executable(test_grid
    test_grid_manager.cpp
)

target_link_libraries(test_grid
    PRIVATE
        pulse::grid
        pulse::market
        pulse::strategy
        pulse::execution
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(test_grid)
