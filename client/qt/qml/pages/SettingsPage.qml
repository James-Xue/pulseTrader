import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Pulse.Client

/// Connection settings and polling controls.
ColumnLayout {
    id: root

    property var controller

    spacing: Style.gap

    GridLayout {
        Layout.fillWidth: true
        Layout.margins: Style.pad
        columns: 2
        columnSpacing: Style.gap
        rowSpacing: Style.gap

        Text { text: qsTr("Host"); color: Style.secondaryText; font.pixelSize: 12 }
        MacTextField { id: hostField; Layout.fillWidth: true; text: "127.0.0.1" }

        Text { text: qsTr("Port"); color: Style.secondaryText; font.pixelSize: 12 }
        MacTextField {
            id: portField
            Layout.fillWidth: true
            text: "8081"
            inputMethodHints: Qt.ImhDigitsOnly
        }

        Text { text: qsTr("TLS"); color: Style.secondaryText; font.pixelSize: 12 }
        MacCheckBox { id: tlsBox; text: qsTr("encrypt the control socket") }

        Text { text: qsTr("Token"); color: Style.secondaryText; font.pixelSize: 12 }
        MacTextField {
            id: tokenField
            Layout.fillWidth: true
            echoMode: TextInput.Password
            placeholderText: qsTr("reserved for M-Q4")
        }
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.leftMargin: Style.pad
        Layout.rightMargin: Style.pad
        spacing: Style.gap

        MacButton {
            text: qsTr("Connect")
            onClicked: {
                if (root.controller) {
                    root.controller.connectToEngine(hostField.text,
                                                    parseInt(portField.text),
                                                    tlsBox.checked,
                                                    tokenField.text)
                }
            }
        }

        MacButton {
            text: qsTr("Disconnect")
            onClicked: if (root.controller) root.controller.disconnectFromEngine()
        }

        Item { Layout.fillWidth: true }

        MacSwitch {
            id: pollingSwitch
            text: qsTr("Polling")
            checked: root.controller ? root.controller.pollingEnabled : true
            onToggled: if (root.controller) root.controller.setPollingEnabled(checked)
        }
    }

    Text {
        Layout.margins: Style.pad
        text: qsTr("State: %1").arg(root.controller
              ? (root.controller.connectionState === EngineController.Ready
                 ? qsTr("ready") : qsTr("not ready"))
              : qsTr("no controller"))
        color: Style.secondaryText
        font.pixelSize: 11
    }

    Item { Layout.fillHeight: true }
}
