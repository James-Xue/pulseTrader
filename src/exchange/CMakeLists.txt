# pulse_exchange — Gate.io v4 REST + WebSocket client with HMAC-SHA512 signing.

add_library(pulse_exchange STATIC
    EndpointRouter.cpp
    gate_auth.cpp
    GateRestClient.cpp
    GateWsChannels.cpp
    GateWsClient.cpp
    ProxyTunnel.cpp
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

# websocketpp: prefer system / vcpkg-installed package; fall back to vendored copy
# in third_party/websocketpp (required on Windows where vcpkg websocketpp is unavailable).
# asio: header-only, installed via vcpkg.
# ASIO_STANDALONE is defined in GateWsClient.cpp before including websocketpp headers.
if(websocketpp_FOUND)
    # vcpkg or system package found (typical on Linux / macOS)
    target_link_libraries(pulse_exchange PRIVATE websocketpp::websocketpp)
elseif(EXISTS "${PROJECT_SOURCE_DIR}/third_party/websocketpp/websocketpp/client.hpp")
    set(WEBSOCKETPP_INCLUDE_DIR "${PROJECT_SOURCE_DIR}/third_party/websocketpp")
    target_include_directories(pulse_exchange PRIVATE ${WEBSOCKETPP_INCLUDE_DIR})
else()
    message(FATAL_ERROR
        "websocketpp not found. Install via vcpkg or place headers in third_party/websocketpp/")
endif()

find_path(ASIO_INCLUDE_DIR asio.hpp)
if(ASIO_INCLUDE_DIR)
    target_include_directories(pulse_exchange PRIVATE ${ASIO_INCLUDE_DIR})
endif()

# Suppress warnings from websocketpp third-party headers (C4127: constant conditional,
# C4267: size_t narrowing, C4702: unreachable code)
if(MSVC)
    target_compile_options(pulse_exchange PRIVATE /wd4127 /wd4267 /wd4702)
endif()

target_compile_features(pulse_exchange PUBLIC cxx_std_20)
