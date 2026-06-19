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

if(PULSE_ENABLE_WEBUI)
    target_compile_definitions(pulsetrader PRIVATE PULSE_ENABLE_WEBUI=1)

    target_include_directories(pulsetrader
        PRIVATE
            ${PROJECT_SOURCE_DIR}/third_party/uWebSockets
            ${PROJECT_SOURCE_DIR}/third_party/uSockets
    )

    target_link_libraries(pulsetrader
        PRIVATE
            pulse::webui
            ${PROJECT_SOURCE_DIR}/third_party/uSockets/uSockets.a
            z
    )
endif()
