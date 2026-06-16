# Layer 6 Strategy Engine — static library
#
# Contains:
#   - strategy_manager: multi-strategy orchestration
#   - scalping/: concrete strategy implementations (momentum, orderbook, mean reversion)
#   - signal/: signal aggregation (weighted voting)

add_library(pulse_strategy STATIC
    strategy_manager.cpp
    scalping/momentum_scalper.cpp
    scalping/orderbook_scalper.cpp
    scalping/mean_reversion_scalper.cpp
    signal/signal_aggregator.cpp
)

add_library(pulse::strategy ALIAS pulse_strategy)

target_include_directories(pulse_strategy
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
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
