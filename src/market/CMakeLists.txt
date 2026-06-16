# pulse_market — Layer 3 Market Data Pipeline (ticker, orderbook, kline, dispatcher)

add_library(pulse_market STATIC
    ticker_cache.cpp
    symbol_registry.cpp
    kline_buffer.cpp
    orderbook_manager.cpp
    market_feed.cpp
)

add_library(pulse::market ALIAS pulse_market)

target_include_directories(pulse_market
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
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
