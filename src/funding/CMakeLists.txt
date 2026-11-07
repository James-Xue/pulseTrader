# pulse_funding — funding-rate window monitor (harvest-window signal)

add_library(pulse_funding STATIC
    FundingWatch.cpp
)

add_library(pulse::funding ALIAS pulse_funding)

target_include_directories(pulse_funding
    PUBLIC
        ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(pulse_funding
    PUBLIC
        pulse::core
        pulse::logging
        pulse::strategy
        pulse::exchange
)

target_compile_features(pulse_funding PUBLIC cxx_std_20)
