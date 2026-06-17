# pulse_webui — Layer 9 WebUI Dashboard

add_library(pulse_webui STATIC
    dashboard_state.cpp
    web_server.cpp
    ws_server.cpp
)

add_library(pulse::webui ALIAS pulse_webui)

target_include_directories(pulse_webui
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
    PRIVATE
        # uWebSockets (header-only C++ wrapper)
        ${PROJECT_SOURCE_DIR}/third_party/uWebSockets
        # uSockets (C library headers for us_listen_socket_close, etc.)
        ${PROJECT_SOURCE_DIR}/third_party/uSockets
)

target_link_libraries(pulse_webui
    PUBLIC
        pulse::core
        pulse::logging
        pulse::market
        pulse::strategy
        pulse::risk
        pulse::execution
        pulse::ai
    PRIVATE
        nlohmann_json::nlohmann_json
        # uSockets static library (C event loop layer behind uWebSockets)
        ${PROJECT_SOURCE_DIR}/third_party/uSockets/uSockets.a
        # zlib for per-message deflate compression support
        z
)

target_compile_features(pulse_webui PUBLIC cxx_std_20)
