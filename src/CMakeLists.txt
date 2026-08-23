add_subdirectory(core)

# Uncomment each module as it is implemented:
add_subdirectory(exchange)
add_subdirectory(market)
add_subdirectory(strategy)
add_subdirectory(heartbeat)
add_subdirectory(ai)
add_subdirectory(risk)
add_subdirectory(execution)
add_subdirectory(logging)
add_subdirectory(control)
add_subdirectory(grid)
add_subdirectory(backtest)

if(PULSE_ENABLE_SQLITE)
    add_subdirectory(trade_recorder)
endif()
