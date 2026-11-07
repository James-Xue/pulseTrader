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
