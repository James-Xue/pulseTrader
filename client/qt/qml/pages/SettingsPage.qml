import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtCore
import Pulse.Client

/// Connection summary, polling controls and the sign-out action.
///
/// The connection itself is configured on the profile page before the engine
/// starts; this page only reports it and offers a clean shutdown (stop the
/// engine process, detach the sockets, wipe the vault key).
ColumnLayout {
    id: root

    property var controller
    property var account
    property var launcher

    spacing: Style.gap

    /// Dashboard preferences. Kept here because this is the only page that
    /// edits them; SettingsPage is instantiated at startup, so the saved
    /// choice is applied before the first account poll.
    Settings {
        id: dashboardPreferences
        category: "dashboard"
        property bool spotHoldings: true
    }

    Component.onCompleted: {
        if (root.controller) {
            root.controller.setSpotHoldingsEnabled(dashboardPreferences.spotHoldings)
        }
    }

    GridLayout {
        Layout.fillWidth: true
        Layout.margins: Style.pad
        columns: 2
        columnSpacing: Style.gap
        rowSpacing: Style.gap

        Text { text: qsTr("Exchange"); color: Style.secondaryText; font.pixelSize: 12 }
        Text { text: qsTr("Gate.io"); color: Style.text; font.pixelSize: 13 }

        Text { text: qsTr("Network"); color: Style.secondaryText; font.pixelSize: 12 }
        Text {
            text: root.account ? root.account.profileNetwork : "—"
            color: Style.text
            font.pixelSize: 13
        }

        Text { text: qsTr("Market"); color: Style.secondaryText; font.pixelSize: 12 }
        Text {
            text: root.account ? root.account.profileMarket : "—"
            color: Style.text
            font.pixelSize: 13
        }

        Text { text: qsTr("Engine process"); color: Style.secondaryText; font.pixelSize: 12 }
        Text {
            text: root.launcher && root.launcher.running ? qsTr("running") : qsTr("stopped")
            color: root.launcher && root.launcher.running ? Style.success : Style.secondaryText
            font.pixelSize: 13
        }

        Text { text: qsTr("Control socket"); color: Style.secondaryText; font.pixelSize: 12 }
        Text {
            text: qsTr("State: %1").arg(root.controller
                  ? (root.controller.connectionState === EngineController.Ready
                     ? qsTr("ready") : qsTr("not ready"))
                  : qsTr("no controller"))
            color: Style.secondaryText
            font.pixelSize: 13
        }
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.leftMargin: Style.pad
        Layout.rightMargin: Style.pad
        spacing: Style.gap

        MacSwitch {
            id: holdingsSwitch
            text: qsTr("Alt-coin spot holdings")
            checked: dashboardPreferences.spotHoldings
            onToggled: {
                dashboardPreferences.spotHoldings = checked
                if (root.controller) {
                    root.controller.setSpotHoldingsEnabled(checked)
                }
            }
        }

        Text {
            Layout.fillWidth: true
            text: qsTr("Prices every held coin with extra REST calls; turn off "
                       + "for faster balance updates.")
            color: Style.tertiaryText
            font.pixelSize: Style.fontSizeSmall
            wrapMode: Text.WordWrap
        }
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.leftMargin: Style.pad
        Layout.rightMargin: Style.pad
        spacing: Style.gap

        MacSwitch {
            id: pollingSwitch
            text: qsTr("Polling")
            checked: root.controller ? root.controller.pollingEnabled : true
            onToggled: if (root.controller) root.controller.setPollingEnabled(checked)
        }

        Item { Layout.fillWidth: true }

        MacButton {
            text: qsTr("Stop engine & sign out")
            onClicked: {
                if (root.launcher) {
                    root.launcher.stop()
                }
                if (root.controller) {
                    root.controller.disconnectFromEngine()
                }
                if (root.account) {
                    root.account.signOut()
                }
            }
        }
    }

    Item { Layout.fillHeight: true }
}
