# src/trade_recorder/CMakeLists.txt — SQLite trade recorder (Phase 2)

add_library(pulse_trade_recorder STATIC
    TradeRecorder.cpp
)

add_library(pulse::trade_recorder ALIAS pulse_trade_recorder)

target_include_directories(pulse_trade_recorder
    PUBLIC
        ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(pulse_trade_recorder
    PUBLIC
        pulse::core
        pulse::execution
    PRIVATE
        SQLiteCpp
        spdlog::spdlog
)

target_compile_features(pulse_trade_recorder PUBLIC cxx_std_20)
