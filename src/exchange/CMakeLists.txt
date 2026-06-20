# pulse_exchange — Gate.io v4 REST + WebSocket client with HMAC-SHA512 signing.

add_library(pulse_exchange STATIC
    endpoint_router.cpp
    gate_auth.cpp
    gate_rest_client.cpp
    gate_ws_channels.cpp
    gate_ws_client.cpp
    proxy_tunnel.cpp
)

add_library(pulse::exchange ALIAS pulse_exchange)

target_include_directories(pulse_exchange
    PUBLIC
        ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(pulse_exchange
    PUBLIC
        pulse::core
        pulse::logging
        nlohmann_json::nlohmann_json
    PRIVATE
        CURL::libcurl
        OpenSSL::SSL
        OpenSSL::Crypto
)

# websocketpp + asio: header-only, add include paths
# ASIO_STANDALONE is defined in gate_ws_client.cpp before including websocketpp headers.
# System-installed headers are at /usr/include (apt: libwebsocketpp-dev, libasio-dev).
find_path(WEBSOCKETPP_INCLUDE_DIR websocketpp/client.hpp)
find_path(ASIO_INCLUDE_DIR asio.hpp)

if(WEBSOCKETPP_INCLUDE_DIR)
    target_include_directories(pulse_exchange PRIVATE ${WEBSOCKETPP_INCLUDE_DIR})
endif()
if(ASIO_INCLUDE_DIR)
    target_include_directories(pulse_exchange PRIVATE ${ASIO_INCLUDE_DIR})
endif()

target_compile_features(pulse_exchange PUBLIC cxx_std_20)
