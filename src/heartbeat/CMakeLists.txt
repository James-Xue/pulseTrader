# Layer 5 Heartbeat Scheduler — static library
#
# Contains:
#   - task_queue: thread-safe task queue with dedicated worker thread
#   - heartbeat_scheduler: asio::steady_timer 5-minute AI analysis clock

add_library(pulse_heartbeat STATIC
    TaskQueue.cpp
    HeartbeatScheduler.cpp
)

add_library(pulse::heartbeat ALIAS pulse_heartbeat)

target_include_directories(pulse_heartbeat
    PUBLIC
        ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(pulse_heartbeat
    PUBLIC
        pulse::core
        pulse::logging
        pulse::ai
        pulse::strategy
)

target_compile_features(pulse_heartbeat PUBLIC cxx_std_20)
