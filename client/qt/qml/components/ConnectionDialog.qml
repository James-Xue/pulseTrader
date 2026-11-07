import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Pulse.Client

/// Endpoint and credentials dialog.
///
/// The token field is a placeholder until M-Q4 adds the auth handshake: the
/// engine currently accepts unauthenticated loopback connections only, so the
/// value is stored but not sent.
Dialog {
    id: root

    property var controller

    title: qsTr("Connect to engine")
    modal: true
    standardButtons: Dialog.NoButton
    width: 420

    ColumnLayout {
        anchors.fill: parent
        spacing: Style.gap

        MacTextField {
            id: hostField
            Layout.fillWidth: true
            text: "127.0.0.1"
            placeholderText: qsTr("host")
        }

        MacTextField {
            id: portField
            Layout.fillWidth: true
            text: "8081"
            placeholderText: qsTr("port")
            inputMethodHints: Qt.ImhDigitsOnly
        }

        MacTextField {
            id: tokenField
            Layout.fillWidth: true
            echoMode: TextInput.Password
            placeholderText: qsTr("token (reserved for M-Q4)")
        }

        MacCheckBox {
            id: tlsBox
            text: qsTr("TLS")
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Style.gap

            MacButton {
                Layout.fillWidth: true
                text: qsTr("Connect")
                onClicked: {
                    if (!root.controller) {
                        return
                    }
                    root.controller.connectToEngine(hostField.text,
                                                    parseInt(portField.text),
                                                    tlsBox.checked,
                                                    tokenField.text)
                    root.close()
                }
            }

            MacButton {
                Layout.fillWidth: true
                text: qsTr("Cancel")
                onClicked: root.close()
            }
        }
    }
}
