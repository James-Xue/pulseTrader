# apps/pulsetrader_ui/CMakeLists.txt — Qt launcher ("模拟 Gate 系统")
#
# Two targets:
#   pulse_ui       — static library with the Qt-free-ish launcher logic
#                    (credential store, verification). Linked by both the
#                    launcher executable and tests/unit/ui.
#   pulsetrader_ui — the GUI executable (login dialog + control window).
#
# The engine targets stay Qt-free; only this launcher links Qt.

# ---------------------------------------------------------------------------
# pulse_ui — launcher logic library (QString-based, no QWidgets)
# ---------------------------------------------------------------------------
add_library(pulse_ui STATIC
    CredentialsStore.cpp
    VerifyWorker.cpp
    SimAccountStore.cpp
    SimTradeEngine.cpp
    GoldStrategyEngine.cpp
)

target_include_directories(pulse_ui
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(pulse_ui
    PUBLIC
        pulse::exchange
        Qt${PULSE_QT_MAJOR}::Core
)

# Qt headers are not warning-clean under /WX (global /W4 + /WX on MSVC).
if(MSVC)
    target_compile_options(pulse_ui PRIVATE /WX-)
else()
    target_compile_options(pulse_ui PRIVATE -Wno-error)
endif()

# ---------------------------------------------------------------------------
# pulsetrader_ui — the GUI executable
# ---------------------------------------------------------------------------
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)  # MainWindow.ui → ui_MainWindow.h

# WIN32 subsystem: no console window for the GUI (the engine itself stays a
# console app — it needs the terminal for its REPL and log output).
add_executable(pulsetrader_ui WIN32
    main.cpp
    LoginDialog.cpp
    MainWindow.cpp
    MainWindow.ui
)

target_link_libraries(pulsetrader_ui
    PRIVATE
        pulse_ui
        Qt${PULSE_QT_MAJOR}::Widgets
)

# The main-window class name is Chinese (模拟gate系统, per user spec) —
# MSVC parses non-ASCII identifiers correctly only with /utf-8.
if(MSVC)
    target_compile_options(pulsetrader_ui PRIVATE /utf-8)
endif()

# The project builds with -Werror (/WX). Qt headers are not warning-clean
# under /W4 (MSVC) or -Werror (GCC/Clang), so soften this target only.
if(MSVC)
    target_compile_options(pulsetrader_ui PRIVATE /WX-)
else()
    target_compile_options(pulsetrader_ui PRIVATE -Wno-error)
endif()

if(MSVC)
    target_compile_definitions(pulsetrader_ui PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
