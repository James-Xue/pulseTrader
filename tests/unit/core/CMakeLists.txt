add_executable(test_core
    testTypes.cpp
)

target_link_libraries(test_core
    PRIVATE
        pulse::core
        GTest::gtest_main
)

add_executable(test_config_loader
    test_config_loader.cpp
)

target_link_libraries(test_config_loader
    PRIVATE
        pulse::core
        GTest::gtest_main
)

add_executable(test_config_validator
    test_config_validator.cpp
)

target_link_libraries(test_config_validator
    PRIVATE
        pulse::core
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(test_core)
gtest_discover_tests(test_config_loader)
gtest_discover_tests(test_config_validator)
