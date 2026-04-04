# pulse_core is a header-only INTERFACE library.
# All other modules link against pulse::core to get the include path
# and C++20 requirement propagated automatically.

add_library(pulse_core INTERFACE)
add_library(pulse::core ALIAS pulse_core)

target_include_directories(pulse_core
    INTERFACE
        ${PROJECT_SOURCE_DIR}/include
)

target_compile_features(pulse_core INTERFACE cxx_std_20)
