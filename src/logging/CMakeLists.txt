# pulse_logging — async per-module logger wrapping spdlog.

add_library(pulse_logging STATIC
    logger.cpp
)

add_library(pulse::logging ALIAS pulse_logging)

target_include_directories(pulse_logging
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(pulse_logging
    PUBLIC
        pulse::core
        spdlog::spdlog
        fmt::fmt
)

target_compile_features(pulse_logging PUBLIC cxx_std_20)
