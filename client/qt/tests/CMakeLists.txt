# Transport tests run against a scripted local QTcpServer: no engine, no
# network. The RPC sources are compiled directly into the test binary because
# the application target is an executable (not linkable) and its QML module
# requires every source to live in the executable.
qt_add_executable(tst_rpcclient
    tst_rpcclient.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/rpc/RpcClient.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/rpc/RpcError.cpp
)

target_include_directories(tst_rpcclient PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src)

target_link_libraries(tst_rpcclient PRIVATE Qt6::Test Qt6::Network)

add_test(NAME rpcclient COMMAND tst_rpcclient)

# Vault cryptography and engine-config generation. The vault test runs against
# a temporary file; the config test never spawns a process.
qt_add_executable(tst_secretstore
    tst_secretstore.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/security/SecretStore.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/engine/EngineLauncher.cpp
)

set_target_properties(tst_secretstore PROPERTIES AUTOMOC ON)

target_include_directories(tst_secretstore PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src)

target_link_libraries(tst_secretstore PRIVATE
    Qt6::Test Qt6::Core Qt6::Qml OpenSSL::Crypto)

add_test(NAME secretstore COMMAND tst_secretstore)

# Indicator math: pure functions over a kline array, no sockets and no engine.
qt_add_executable(tst_indicators
    tst_indicators.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/data/IndicatorEngine.cpp
)

target_include_directories(tst_indicators PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src)

target_link_libraries(tst_indicators PRIVATE Qt6::Test Qt6::Core)

add_test(NAME indicators COMMAND tst_indicators)

# Data-server protocol: listener, sessions and NDJSON framing against an
# ephemeral loopback port. pollEngine is false in the test config, so the
# bundled MarketDataService never opens a socket to a live engine.
qt_add_executable(tst_dataserver
    tst_dataserver.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/data/DataServer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/data/DataSession.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/data/MarketDataService.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/data/IndicatorEngine.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/rpc/RpcClient.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/rpc/RpcError.cpp
)

target_include_directories(tst_dataserver PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src)

# Qt6::Qml is required because DataServer.hpp is a QML_ELEMENT header and
# pulls in qqmlregistration.h; this target generates no QML module.
target_link_libraries(tst_dataserver PRIVATE
    Qt6::Test Qt6::Core Qt6::Network Qt6::Qml)

add_test(NAME dataserver COMMAND tst_dataserver)
