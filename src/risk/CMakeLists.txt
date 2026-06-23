# pulse_risk — Layer 7 Risk Management (position tracking, order gating, stop/profit engines)

add_library(pulse_risk STATIC
    PositionManager.cpp
    DrawdownGuard.cpp
    OrderRateLimiter.cpp
    RiskManager.cpp
    StopLossEngine.cpp
    TakeProfitEngine.cpp
)

add_library(pulse::risk ALIAS pulse_risk)

target_include_directories(pulse_risk
    PUBLIC
        ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(pulse_risk
    PUBLIC
        pulse::core
        pulse::logging
    PRIVATE
        pulse::execution
)

target_compile_features(pulse_risk PUBLIC cxx_std_20)
