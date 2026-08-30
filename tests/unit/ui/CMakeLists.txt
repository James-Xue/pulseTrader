# tests/unit/ui/CMakeLists.txt — unit tests for the Qt launcher logic (pulse_ui)
#
# Built only when the launcher itself is enabled (PULSE_BUILD_UI + Qt found —
# both decided in the top-level CMakeLists before tests are added).

add_executable(test_ui
    test_credentials_store.cpp
    test_verify_worker.cpp
    test_sim_account_store.cpp
    test_sim_trade_engine.cpp
    test_gold_strategy.cpp
)

target_link_libraries(test_ui
    PRIVATE
        pulse_ui
        GTest::gtest_main
)

# Qt headers are not warning-clean under the global /WX (MSVC) / -Werror.
if(MSVC)
    target_compile_options(test_ui PRIVATE /WX-)
else()
    target_compile_options(test_ui PRIVATE -Wno-error)
endif()
