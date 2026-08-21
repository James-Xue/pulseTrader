# Layer 6 Strategy Engine — static library
#
# Contains:
#   - strategy_manager: multi-strategy orchestration
#   - scalping/: concrete strategy implementations (momentum, orderbook, mean reversion, supertrend)
#   - signal/: signal aggregation (weighted voting)

add_library(pulse_strategy STATIC
    StrategyManager.cpp
    StrategyRegistry.cpp
    scalping/UnifiedScalper.cpp
    scalping/MomentumScalper.cpp
    scalping/OrderBookScalper.cpp
    scalping/MeanReversionScalper.cpp
    scalping/SuperTrendScalper.cpp
    scalping/EthScalper.cpp
    signal/SignalAggregator.cpp
    signal/SignalBoard.cpp
)

add_library(pulse::strategy ALIAS pulse_strategy)

target_include_directories(pulse_strategy
    PUBLIC
        ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(pulse_strategy
    PUBLIC
        pulse::core
        pulse::logging
        pulse::market
        pulse::risk
        pulse::execution
)

target_compile_features(pulse_strategy PUBLIC cxx_std_20)
