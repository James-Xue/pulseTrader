add_executable(test_ai_unit
    test_analysis_result.cpp
    test_prompt_builder.cpp
    test_ai_client.cpp
    test_param_advisor.cpp
    test_ai_pipeline.cpp
)

target_link_libraries(test_ai_unit
    PRIVATE
        pulse::ai
        pulse::strategy
        pulse::logging
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(test_ai_unit)
