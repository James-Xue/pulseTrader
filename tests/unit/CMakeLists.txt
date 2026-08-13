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

if(PULSE_ENABLE_SQLITE)
    add_subdirectory(trade_recorder)
endif()
