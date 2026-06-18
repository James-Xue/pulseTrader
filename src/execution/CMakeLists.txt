add_library(pulse_execution STATIC
    execution_report.cpp
    order_executor.cpp
    order_tracker.cpp
)

add_library(pulse::execution ALIAS pulse_execution)

target_include_directories(pulse_execution
    PUBLIC
        ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(pulse_execution
    PUBLIC
        pulse::core
        pulse::exchange
        pulse::market
    PRIVATE
        nlohmann_json::nlohmann_json
)

target_compile_features(pulse_execution PUBLIC cxx_std_20)
