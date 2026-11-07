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
