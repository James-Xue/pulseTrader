cmake_minimum_required(VERSION 3.21)

# Standalone-first: when the top-level build adds this directory, project() has
# already run and must not run again.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(pulseTraderQt VERSION 0.1.0 LANGUAGES CXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Qt 6.12 LTS. Test and QuickTest are separate packages and are only needed for
# the test targets below.
find_package(Qt6 6.12 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Network)

# AES-256-GCM and PBKDF2 for the local vault. Qt has no GCM primitive, and the
# engine already depends on OpenSSL, so this adds no new third-party library.
find_package(OpenSSL REQUIRED)

# Enables AUTOMOC and policy QTP0001, which gives QML files the default
# resource prefix /qt/qml so loadFromModule() works without an import path.
qt_standard_project_setup(REQUIRES 6.5)

# A pragma Singleton QML file must be declared or qmlcachegen rejects it.
set_source_files_properties(qml/Style.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_executable(pulseTraderQt MANUAL_FINALIZATION
    main.cpp
)

qt_add_qml_module(pulseTraderQt
    URI Pulse.Client
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/Style.qml
        qml/pages/LoginPage.qml
        qml/pages/ProfilePage.qml
        qml/pages/DashboardPage.qml
        qml/pages/PositionsPage.qml
        qml/pages/OrdersPage.qml
        qml/pages/SignalsPage.qml
        qml/pages/MarketPage.qml
        qml/pages/SettingsPage.qml
        qml/components/StatusBar.qml
        qml/components/MetricTile.qml
        qml/components/StaleBadge.qml
        qml/components/ErrorBanner.qml
        qml/components/SignalCard.qml
        qml/components/EmptyState.qml
        qml/components/TrafficLights.qml
        qml/components/Sidebar.qml
        qml/components/WindowResizer.qml
        qml/components/ThemeToggle.qml
        qml/components/MacTextField.qml
        qml/components/MacButton.qml
        qml/components/MacCheckBox.qml
        qml/components/MacSwitch.qml
        qml/components/MacComboBox.qml
        qml/components/MacSegmentedControl.qml
    SOURCES
        src/rpc/RpcClient.cpp          src/rpc/RpcClient.hpp
        src/rpc/RpcError.cpp           src/rpc/RpcError.hpp
        src/rpc/RpcMethods.hpp
        src/security/SecretStore.cpp   src/security/SecretStore.hpp
        src/account/AccountService.cpp src/account/AccountService.hpp
        src/engine/EngineController.cpp src/engine/EngineController.hpp
        src/engine/EngineLauncher.cpp  src/engine/EngineLauncher.hpp
        src/models/JsonListModel.cpp   src/models/JsonListModel.hpp
        src/models/PositionsModel.hpp
        src/models/OrdersModel.hpp
        src/models/ReportsModel.hpp
        src/models/SignalsModel.hpp
        src/models/AccountModel.cpp    src/models/AccountModel.hpp
    RESOURCES
        resources/icons/app.svg
)

# src/ is the public include root. The per-module directories are also on the
# path because the generated QML type registration file includes each
# QML_ELEMENT header by bare file name (`#include <EngineController.hpp>`).
target_include_directories(pulseTraderQt PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/account
    ${CMAKE_CURRENT_SOURCE_DIR}/src/engine
    ${CMAKE_CURRENT_SOURCE_DIR}/src/models
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rpc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/security)

target_link_libraries(pulseTraderQt PRIVATE
    Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Network
    OpenSSL::Crypto)

set_target_properties(pulseTraderQt PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_GUI_IDENTIFIER com.pulsetrader.client
    MACOSX_BUNDLE_BUNDLE_NAME pulseTrader)

# Android application id and minSdk are target properties declared in
# lib/cmake/Qt6Core/Qt6AndroidMacros.cmake, which only exists inside a Qt for
# Android kit. A desktop configure silently ignores them.
if(ANDROID)
    set_target_properties(pulseTraderQt PROPERTIES
        QT_ANDROID_PACKAGE_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/android
        QT_ANDROID_PACKAGE_NAME com.pulsetrader.client
        QT_ANDROID_MIN_SDK_VERSION 28)
endif()

if(NOT ANDROID)
    include(GNUInstallDirs)
    install(TARGETS pulseTraderQt
        BUNDLE  DESTINATION .
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# Qt requires finalization immediately BEFORE the deploy script is generated.
qt_finalize_target(pulseTraderQt)

if(NOT ANDROID)
    qt_generate_deploy_qml_app_script(
        TARGET pulseTraderQt
        OUTPUT_SCRIPT deploy_script
        NO_UNSUPPORTED_PLATFORM_ERROR)
    install(SCRIPT ${deploy_script})
endif()

option(PULSE_QT_BUILD_TESTS "Build Qt client tests" ON)
if(PULSE_QT_BUILD_TESTS)
    enable_testing()
    find_package(Qt6 6.12 REQUIRED COMPONENTS Test)
    add_subdirectory(tests)
endif()
