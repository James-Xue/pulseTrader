# pulse_webui — Layer 9 WebUI Dashboard

find_package(ZLIB REQUIRED)
find_package(unofficial-usockets CONFIG REQUIRED)
find_package(unofficial-uwebsockets CONFIG REQUIRED)

add_library(pulse_webui STATIC
    DashboardState.cpp
    WebServer.cpp
    WsServer.cpp
)

add_library(pulse::webui ALIAS pulse_webui)

target_include_directories(pulse_webui
    PUBLIC
        ${PROJECT_SOURCE_DIR}/src
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
        unofficial::uwebsockets::uwebsockets
        unofficial::usockets::usockets
        ZLIB::ZLIB
)

target_compile_features(pulse_webui PUBLIC cxx_std_20)

# Suppress warnings from uWebSockets/uSockets third-party headers
if(MSVC)
    target_compile_options(pulse_webui PRIVATE /wd4005 /wd4456 /wd4458 /WX-)
endif()
