# pulse_market — Layer 3 Market Data Pipeline (ticker, orderbook, kline, dispatcher)

add_library(pulse_market STATIC
    TickerCache.cpp
    SymbolRegistry.cpp
    KlineBuffer.cpp
    OrderBookManager.cpp
    MarketFeed.cpp
)

add_library(pulse::market ALIAS pulse_market)

target_include_directories(pulse_market
    PUBLIC
        ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(pulse_market
    PUBLIC
        pulse::core
        pulse::logging
        pulse::exchange
    PRIVATE
        nlohmann_json::nlohmann_json
)

target_compile_features(pulse_market PUBLIC cxx_std_20)
