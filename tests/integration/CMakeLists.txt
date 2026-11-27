# tests/integration — cross-layer flows.
#
# The paper engine flow needs the SQLite trade recorder (history + PnL rows),
# so the target exists only in SQLite builds. P5 of the 2026-11-27 audit
# enabled this directory; previously it was commented out of tests/CMakeLists.
add_executable(test_integration_paper_flow
    test_paper_engine_flow.cpp
)
target_link_libraries(test_integration_paper_flow
    PRIVATE
        pulse::control
        pulse::risk
        pulse::execution
        pulse::exchange
        pulse::core
        pulse::strategy
        pulse::logging
        pulse::trade_recorder
        GTest::gtest_main
)
target_compile_definitions(test_integration_paper_flow
    PRIVATE PULSE_ENABLE_SQLITE=1)
include(GoogleTest)
gtest_discover_tests(test_integration_paper_flow)
