cmake_minimum_required(VERSION 3.25)

# vcpkg toolchain — must be set before project()
if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "Vcpkg toolchain file")
endif()

project(pulseTrader
    VERSION     0.1.0
    DESCRIPTION "AI-driven scalping framework with adaptive strategy tuning via real-time social signals"
    LANGUAGES   CXX
)

# ---------------------------------------------------------------------------
# Global C++ standard
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD          20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Build options
# ---------------------------------------------------------------------------
option(PULSE_BUILD_TESTS   "Build unit and integration tests" ON)
option(PULSE_BUILD_DEMOS   "Build demo programs"              OFF)
option(PULSE_ENABLE_SQLITE "Enable SQLite trade recorder"     OFF)
option(PULSE_ENABLE_TOML   "Enable TOML config file support"  OFF)

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------
include(cmake/compiler_flags.cmake)

# ---------------------------------------------------------------------------
# Third-party dependencies
# ---------------------------------------------------------------------------
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog        CONFIG REQUIRED)
find_package(fmt           CONFIG REQUIRED)
find_package(CURL          REQUIRED)
find_package(OpenSSL       REQUIRED)

# asio and websocketpp ship headers only; CONFIG mode may not be available
# on all vcpkg triples — fall back to finding the include path manually.
find_package(asio        CONFIG QUIET)
find_package(websocketpp CONFIG QUIET)

if(PULSE_BUILD_TESTS)
    find_package(GTest CONFIG REQUIRED)
    enable_testing()
endif()

if(PULSE_ENABLE_SQLITE)
    find_package(SQLiteCpp CONFIG REQUIRED)
endif()

if(PULSE_ENABLE_TOML)
    find_package(toml11 CONFIG REQUIRED)
endif()

# ---------------------------------------------------------------------------
# Subdirectories
# ---------------------------------------------------------------------------
add_subdirectory(src)

if(PULSE_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(PULSE_BUILD_DEMOS)
    add_subdirectory(demos)
endif()

add_subdirectory(apps)
