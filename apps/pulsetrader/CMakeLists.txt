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
    find_package(unofficial-usockets CONFIG REQUIRED)
    find_package(unofficial-uwebsockets CONFIG REQUIRED)

    target_compile_definitions(pulsetrader PRIVATE PULSE_ENABLE_WEBUI=1)

    target_link_libraries(pulsetrader
        PRIVATE
            pulse::webui
            unofficial::uwebsockets::uwebsockets
            unofficial::usockets::usockets
            ZLIB::ZLIB
    )
endif()

if(PULSE_ENABLE_SQLITE)
    target_compile_definitions(pulsetrader PRIVATE PULSE_ENABLE_SQLITE=1)
    target_link_libraries(pulsetrader PRIVATE pulse::trade_recorder)
endif()
