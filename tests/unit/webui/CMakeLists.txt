find_package(unofficial-usockets CONFIG REQUIRED)
find_package(unofficial-uwebsockets CONFIG REQUIRED)

add_executable(test_webui
    test_snapshot_types.cpp
    test_dashboard_state.cpp
    test_web_server.cpp
    test_ws_server.cpp
)

target_link_libraries(test_webui
    PRIVATE
        pulse::webui
        pulse::market
        pulse::strategy
        pulse::risk
        pulse::execution
        pulse::ai
        unofficial::uwebsockets::uwebsockets
        unofficial::usockets::usockets
        ZLIB::ZLIB
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(test_webui)
