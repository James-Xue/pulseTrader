import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Pulse.Client

/// Root window.
///
/// EngineController is instantiated here rather than injected from main.cpp:
/// it is a QML_ELEMENT in this module, which keeps the C++ entry point free of
/// view wiring and lets every page reach it through one property.
ApplicationWindow {
    id: root

    width: 1100
    height: 720
    visible: true
    title: qsTr("pulseTrader — read-only console")
    color: Style.bg

    EngineController {
        id: engine
    }

    header: StatusBar {
        controller: engine
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        ErrorBanner {
            Layout.fillWidth: true
            message: engine.lastError
            visible: engine.lastError.length > 0
        }

        TabBar {
            id: tabs
            Layout.fillWidth: true

            TabButton { text: qsTr("Dashboard") }
            TabButton { text: qsTr("Positions") }
            TabButton { text: qsTr("Orders") }
            TabButton { text: qsTr("Signals") }
            TabButton { text: qsTr("Market") }
            TabButton { text: qsTr("Settings") }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: tabs.currentIndex

            DashboardPage { controller: engine }
            PositionsPage { controller: engine }
            OrdersPage { controller: engine }
            SignalsPage { controller: engine }
            MarketPage { controller: engine }
            SettingsPage { controller: engine }
        }
    }

    ConnectionDialog {
        id: connectionDialog
        controller: engine
    }

    Connections {
        target: engine

        function onConnectionStateChanged() {
            if (engine.connectionState === EngineController.Disconnected) {
                connectionDialog.open()
            }
        }
    }

    Component.onCompleted: {
        // PULSE_CLIENT_HOST auto-connects from C++; only prompt when the
        // controller did not already take that path.
        if (!engine.autoConnect) {
            connectionDialog.open()
        }
    }
}
