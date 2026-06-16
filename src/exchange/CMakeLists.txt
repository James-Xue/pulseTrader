# pulse_exchange — Gate.io v4 REST client with HMAC-SHA512 signing.

add_library(pulse_exchange STATIC
    gate_auth.cpp
    gate_rest_client.cpp
)

add_library(pulse::exchange ALIAS pulse_exchange)

target_include_directories(pulse_exchange
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
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

target_compile_features(pulse_exchange PUBLIC cxx_std_20)
