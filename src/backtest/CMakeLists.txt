# src/backtest/CMakeLists.txt — backtest engine (M29, single-strategy MVP)

add_library(pulse_backtest STATIC
    BacktestAccount.cpp
    BacktestEngine.cpp
    BacktestReport.cpp
    GateKlineFetcher.cpp
    KlineLoader.cpp
    ReplayDriver.cpp
    WarmupSeeder.cpp
)

add_library(pulse::backtest ALIAS pulse_backtest)

target_include_directories(pulse_backtest
    PUBLIC
        ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(pulse_backtest
    PUBLIC
        pulse::core
        pulse::exchange
        pulse::market
        pulse::strategy
    PRIVATE
        spdlog::spdlog
)

# SqliteKlineReader is optional (SQLite is a build-time option). Without it
# the loader degrades to a pure Gate API fetch — the header is safe either way.
# The define reaches BacktestEngine (sqlite-source wiring) and the reader TU.
if(PULSE_ENABLE_SQLITE)
    target_sources(pulse_backtest PRIVATE SqliteKlineReader.cpp)
    target_compile_definitions(pulse_backtest PRIVATE PULSE_ENABLE_SQLITE)
    target_link_libraries(pulse_backtest PRIVATE SQLiteCpp)
endif()

target_compile_features(pulse_backtest PUBLIC cxx_std_20)
