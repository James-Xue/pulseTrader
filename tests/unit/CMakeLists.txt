add_subdirectory(core)

# Uncomment as each module gets unit tests:
add_subdirectory(exchange)
add_subdirectory(execution)
add_subdirectory(logging)
add_subdirectory(market)
add_subdirectory(strategy)
add_subdirectory(risk)
add_subdirectory(ai)
add_subdirectory(heartbeat)
add_subdirectory(control)
add_subdirectory(grid)
add_subdirectory(backtest)

if(PULSE_ENABLE_SQLITE)
    add_subdirectory(trade_recorder)
endif()

# Qt launcher tests — only when the launcher is enabled (PULSE_QT_MAJOR is
# set by the top-level CMakeLists before tests are added).
if(PULSE_BUILD_UI AND PULSE_QT_MAJOR)
    add_subdirectory(ui)
endif()
