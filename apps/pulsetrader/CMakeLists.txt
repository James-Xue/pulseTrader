# apps/pulsetrader/CMakeLists.txt — pulseTrader trading engine executable
#
# Wires all 9 layers into a single trading process.

add_executable(pulsetrader
    main.cpp
)

target_link_libraries(pulsetrader
    PRIVATE
        pulse::strategy
        pulse::market
        pulse::risk
        pulse::execution
        pulse::ai
        pulse::heartbeat
        pulse::exchange
        pulse::logging
        pulse::core
)

target_link_libraries(pulsetrader PRIVATE pulse::control)

if(PULSE_ENABLE_SQLITE)
    target_compile_definitions(pulsetrader PRIVATE PULSE_ENABLE_SQLITE=1)
    target_link_libraries(pulsetrader PRIVATE pulse::trade_recorder)
endif()
