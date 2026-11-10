# DependenciesFetch.cmake — third-party dependencies without vcpkg
#
# Enabled with -DPULSE_USE_FETCHCONTENT=ON. Every dependency here is either
# header-only or small enough to build in a couple of minutes, so a Windows
# machine with only CMake + MSVC + Qt can build the engine without installing
# a package manager.
#
# Targets created here match the system-package names the rest of the tree
# already links against (fmt::fmt, spdlog::spdlog, nlohmann_json::nlohmann_json,
# toml11::toml11, GTest::gtest_main), so no source file changes.
#
# CURL and OpenSSL are NOT fetched: both are large C libraries that need a
# platform-specific build. Point CMake at prebuilt binaries instead:
#   -DCURL_ROOT=<dir>  -DOPENSSL_ROOT_DIR=<dir>
#
# Pinned tags are deliberate. Bumping one is a dependency change, not a chore:
# build it and run the suite before merging.

include(FetchContent)

set(FETCHCONTENT_QUIET OFF)

# --- fmt ---------------------------------------------------------------------
# Kept external so spdlog links the same copy (see SPDLOG_FMT_EXTERNAL below);
# two copies of fmt in one binary is an ODR hazard.
FetchContent_Declare(fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG        10.2.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(fmt)

# --- spdlog ------------------------------------------------------------------
set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "" FORCE)
FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.14.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(spdlog)

# --- nlohmann/json (header only) --------------------------------------------
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# --- toml11 (header only) ----------------------------------------------------
FetchContent_Declare(toml11
    GIT_REPOSITORY https://github.com/ToruNiina/toml11.git
    GIT_TAG        v4.0.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(toml11)

# --- asio (header only) ------------------------------------------------------
# The tree consumes a plain include directory (PULSE_ASIO_INCLUDE_DIR) rather
# than a target, so only that variable is set here.
FetchContent_Declare(asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG        asio-1-30-2
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(asio)
set(PULSE_ASIO_INCLUDE_DIR "${asio_SOURCE_DIR}/asio/include" CACHE PATH
    "asio include directory (FetchContent)" FORCE)
if(NOT EXISTS "${PULSE_ASIO_INCLUDE_DIR}/asio.hpp")
    message(FATAL_ERROR
        "FetchContent asio did not produce asio.hpp at ${PULSE_ASIO_INCLUDE_DIR}")
endif()

# --- websocketpp (header only) ----------------------------------------------
# The copy under third_party/ predates get_io_context(), which GateWsClient.cpp
# calls, so it cannot build this tree. Linux builds have always used the
# distro package (0.8.2); fetch that same release so Windows matches.
FetchContent_Declare(websocketpp
    GIT_REPOSITORY https://github.com/zaphoyd/websocketpp.git
    GIT_TAG        0.8.2
    GIT_SHALLOW    TRUE
    # Header-only: never configure the upstream project. Its CMakeLists still
    # asks for compatibility with CMake < 3.5, which CMake 4.x removed, and
    # there is nothing in it this build needs.
    SOURCE_SUBDIR  cmake-not-used
)
FetchContent_MakeAvailable(websocketpp)
set(PULSE_WEBSOCKETPP_INCLUDE_DIR "${websocketpp_SOURCE_DIR}" CACHE PATH
    "websocketpp include directory (FetchContent)" FORCE)
if(NOT EXISTS "${PULSE_WEBSOCKETPP_INCLUDE_DIR}/websocketpp/client.hpp")
    message(FATAL_ERROR
        "FetchContent websocketpp did not produce websocketpp/client.hpp")
endif()

# --- GoogleTest (only when tests are enabled) --------------------------------
if(PULSE_BUILD_TESTS)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.14.0
        GIT_SHALLOW    TRUE
    )
    # Do not install gtest with the project.
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

message(STATUS "pulseTrader: dependencies resolved via FetchContent "
               "(fmt ${fmt_VERSION}, spdlog ${spdlog_VERSION})")

# Third-party headers must not be held to this project's -Werror (fmt alone
# trips -Wtautological-compare under -O3). The SYSTEM target property (CMake
# 3.25+) makes their include directories system includes for every consumer.
foreach(dep_target IN ITEMS fmt spdlog nlohmann_json toml11
                           gtest gtest_main)
    if(TARGET ${dep_target})
        set_target_properties(${dep_target} PROPERTIES SYSTEM TRUE)
    endif()
endforeach()
