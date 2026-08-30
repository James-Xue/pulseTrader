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
find_package(toml11        CONFIG REQUIRED)

# asio and websocketpp ship headers only; CONFIG mode may not be available
# on all vcpkg triples — fall back to finding the include path manually.
# asio include dir is hoisted so exchange and control targets share it.
find_package(asio        CONFIG QUIET)
find_package(websocketpp CONFIG QUIET)

find_path(PULSE_ASIO_INCLUDE_DIR asio.hpp)
if(NOT PULSE_ASIO_INCLUDE_DIR)
    message(FATAL_ERROR "asio.hpp not found. Install via vcpkg or apt (libasio-dev).")
endif()

if(PULSE_BUILD_TESTS)
    find_package(GTest CONFIG REQUIRED)
    enable_testing()
endif()

if(PULSE_ENABLE_SQLITE)
    find_package(SQLiteCpp CONFIG REQUIRED)
endif()

# ---------------------------------------------------------------------------
# Qt launcher UI (optional) — pulsetrader_ui
# ---------------------------------------------------------------------------
# A separate GUI process that verifies API credentials and manages the
# engine's lifecycle. Skipped when Qt is not installed; the engine itself
# never links Qt. Qt is expected from a system installation (e.g. the Qt
# online installer), NOT from vcpkg — vcpkg Qt is large and slow to build.
#
# Detected here (before the tests subdirectory) so PULSE_QT_MAJOR is also
# visible to tests/unit/ui.
option(PULSE_BUILD_UI "Build the Qt launcher UI (pulsetrader_ui)" ON)
if(PULSE_BUILD_UI)
    # Auto-detect the Qt online-installer layout used on this machine.
    if(NOT DEFINED Qt6_DIR AND NOT DEFINED Qt5_DIR)
        if(EXISTS "C:/Qt/6.5.3/msvc2019_64/lib/cmake/Qt6")
            set(Qt6_DIR "C:/Qt/6.5.3/msvc2019_64/lib/cmake/Qt6" CACHE PATH
                "Qt6 cmake directory (auto-detected)")
        elseif(EXISTS "C:/Qt/5.15.2/msvc2019_64/lib/cmake/Qt5")
            set(Qt5_DIR "C:/Qt/5.15.2/msvc2019_64/lib/cmake/Qt5" CACHE PATH
                "Qt5 cmake directory (auto-detected)")
        endif()
    endif()

    find_package(Qt6 QUIET COMPONENTS Widgets)
    if(Qt6_FOUND)
        set(PULSE_QT_MAJOR 6)
    else()
        find_package(Qt5 QUIET COMPONENTS Widgets)
        if(Qt5_FOUND)
            set(PULSE_QT_MAJOR 5)
        endif()
    endif()

    if(PULSE_QT_MAJOR)
        add_subdirectory(apps/pulsetrader_ui)
        message(STATUS "pulsetrader_ui: Qt${PULSE_QT_MAJOR} launcher enabled")
    else()
        message(WARNING
            "Qt not found — pulsetrader_ui skipped. Set Qt6_DIR / CMAKE_PREFIX_PATH "
            "(e.g. -DQt6_DIR=C:/Qt/6.5.3/msvc2019_64/lib/cmake/Qt6) or disable "
            "with -DPULSE_BUILD_UI=OFF.")
    endif()
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

add_subdirectory(tools)
add_subdirectory(apps)

# ---------------------------------------------------------------------------
# Solution folders (Visual Studio filters) — MVC layout
# ---------------------------------------------------------------------------
# Organises every project in the generated .sln into four solution filters
# (pure presentation: physical layout and build behaviour are unchanged).
#
# This block MUST run after every add_subdirectory — FOLDER assignment for a
# target requires the target to exist already.
#
#   model   — data & credentials layer: core types/config, exchange & market
#             data, klines (backtest/SQLite), logging, credential store (the
#             login key/secret) — where future log exports and kline output
#             live
#   view    — display layer: Qt launcher GUI (a future WebUI lands here too)
#   control — strategy & control plane: strategies, grid, AI tuning, risk,
#             execution, heartbeat, JSON-RPC/MCP control plane
#   main    — program entry points: engine executable + CLI/smoke tools
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# model
foreach(t pulse_core pulse_exchange pulse_market pulse_backtest pulse_logging
         pulse_ui pulse_trade_recorder
         test_core test_config_loader test_config_validator test_param_change_log
         test_exchange test_market test_logging test_backtest test_ui)
    if(TARGET ${t})
        set_property(TARGET ${t} PROPERTY FOLDER model)
    endif()
endforeach()

# view
foreach(t pulsetrader_ui)
    set_property(TARGET ${t} PROPERTY FOLDER view)
endforeach()

# control
foreach(t pulse_strategy pulse_grid pulse_ai pulse_heartbeat pulse_risk
         pulse_execution pulse_control
         test_strategy_unit test_grid test_risk
         test_control_client test_control_command_parser test_control_engine_services
         test_control_json_rpc test_control_mcp test_control_order_flow
         test_execution_unit test_heartbeat_unit test_ai_unit)
    if(TARGET ${t})
        set_property(TARGET ${t} PROPERTY FOLDER control)
    endif()
endforeach()

# main — engine entry point plus the standalone CLI/smoke tools (tools/)
foreach(t pulsetrader backtest_data_check test_ai_pipeline test_balance_diag
         test_control_smoke test_execution test_gate_rest test_gate_ws
         test_market_feed test_strategy)
    if(TARGET ${t})
        set_property(TARGET ${t} PROPERTY FOLDER main)
    endif()
endforeach()

# Startup project for Visual Studio: the engine in the main solution (the UI
# is a view module, not an entry point); the build-all target in the test
# solution (no single test project is a sensible default).
if(PULSE_BUILD_TESTS)
    set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT ALL_BUILD)
else()
    set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT pulsetrader)
endif()
