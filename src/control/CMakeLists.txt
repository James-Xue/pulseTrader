# src/control — Control plane: JSON-RPC socket + CLI REPL + MCP stdio server
# ---------------------------------------------------------------------------
# Exposes the trading engine over:
#   1. OrderFlowExecutor — extracted order execution flow (aggregator + manual)
#   2. EngineServices    — thread-safe service bundle for all control methods
#   3. JsonRpcServer     — TCP control socket (newline-delimited JSON-RPC 2.0)
#   4. ControlClient     — blocking client for cli/mcp subcommands
#   5. CommandParser     — REPL command grammar shared by embedded/remote CLI
#   6. McpServer         — stdio MCP (Model Context Protocol) server
# ---------------------------------------------------------------------------
add_library(pulse_control STATIC
    OrderFlowExecutor.cpp
    EngineServices.cpp
    JsonRpcServer.cpp
    ControlClient.cpp
    CommandParser.cpp
    McpServer.cpp
)
add_library(pulse::control ALIAS pulse_control)

target_include_directories(pulse_control
    PUBLIC ${PROJECT_SOURCE_DIR}/src
    PRIVATE ${PULSE_ASIO_INCLUDE_DIR}
)

target_link_libraries(pulse_control
    PUBLIC
        pulse::core
        pulse::logging
        pulse::strategy
        pulse::risk
        pulse::execution
        pulse::market
        pulse::exchange
        nlohmann_json::nlohmann_json
    PRIVATE
        spdlog::spdlog
        fmt::fmt
)

if(PULSE_ENABLE_SQLITE)
    target_link_libraries(pulse_control PRIVATE pulse::trade_recorder)
endif()

target_compile_features(pulse_control PUBLIC cxx_std_20)
