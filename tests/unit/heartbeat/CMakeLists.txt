add_executable(test_heartbeat_unit
    test_task_queue.cpp
    test_heartbeat_scheduler.cpp
)

target_link_libraries(test_heartbeat_unit
    PRIVATE
        pulse::heartbeat
        pulse::ai
        pulse::strategy
        pulse::logging
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(test_heartbeat_unit)
