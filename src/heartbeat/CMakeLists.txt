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

# asio is header-only and drives HeartbeatScheduler's steady_timer. On Linux it
# lives in /usr/include, so no include path is spelled out and the dependency
# stayed invisible; the Windows FetchContent build supplies it as a plain
# directory, so it must be wired up explicitly.
#
# PUBLIC, not PRIVATE: HeartbeatScheduler.hpp is a public header that includes
# <asio/io_context.hpp>, so every consumer target (apps/pulsetrader) needs the
# same path. The same applies to pulse_control and pulse_exchange.
if(PULSE_ASIO_INCLUDE_DIR)
    target_include_directories(pulse_heartbeat SYSTEM PUBLIC ${PULSE_ASIO_INCLUDE_DIR})
endif()

target_link_libraries(pulse_heartbeat
    PUBLIC
        pulse::core
        pulse::logging
        pulse::ai
        pulse::strategy
)

target_compile_features(pulse_heartbeat PUBLIC cxx_std_20)
