# src/backtest/CMakeLists.txt — backtest engine (M29, single-strategy MVP)

add_library(pulse_backtest STATIC
    BacktestAccount.cpp
    BacktestEngine.cpp
    BacktestReport.cpp
    DailyKlineSync.cpp
    GateKlineFetcher.cpp
    KlineLoader.cpp
    QuantoResolver.cpp
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

# SqliteKlineReader/KlineStore are optional (SQLite is a build-time option).
# Without it the loader degrades to a pure Gate API fetch — the headers are
# safe either way. The define reaches BacktestEngine (sqlite-source wiring),
# the reader TU and the store TU (importFromDb reads raw SQLite).
if(PULSE_ENABLE_SQLITE)
    target_sources(pulse_backtest PRIVATE
        SqliteKlineReader.cpp
        KlineStore.cpp
    )
    target_compile_definitions(pulse_backtest PRIVATE PULSE_ENABLE_SQLITE)
    target_link_libraries(pulse_backtest PRIVATE SQLiteCpp)
endif()

target_compile_features(pulse_backtest PUBLIC cxx_std_20)
