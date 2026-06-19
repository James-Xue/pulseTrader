# pulse_core — core types, error codes, config structs, TOML loader, and validator.
#
# Converted from INTERFACE (header-only) to STATIC to support compiled
# config_loader and config_validator translation units.
# All other modules link against pulse::core to get the include path,
# C++20 requirement, and toml11 dependency propagated automatically.

add_library(pulse_core STATIC
    config_loader.cpp
    config_validator.cpp
)

add_library(pulse::core ALIAS pulse_core)

target_include_directories(pulse_core
    PUBLIC
        ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(pulse_core
    PUBLIC
        toml11::toml11
)

target_compile_features(pulse_core PUBLIC cxx_std_20)
