# pulse_grid — M27 engine-native grid execution service

add_library(pulse_grid STATIC
    GridManager.cpp
)

add_library(pulse::grid ALIAS pulse_grid)

target_include_directories(pulse_grid
    PUBLIC
        ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(pulse_grid
    PUBLIC
        pulse::core
        pulse::logging
        pulse::market
        pulse::strategy
)

target_compile_features(pulse_grid PUBLIC cxx_std_20)
