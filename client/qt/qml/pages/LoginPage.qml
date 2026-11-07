import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtCore
import Pulse.Client

/// Local account gate.
///
/// First run shows the account-creation form; later runs show the unlock form.
/// The password derives an AES-256-GCM key that protects the vault holding the
/// Gate.io API credentials. Main.qml switches to the profile page as soon as
/// `account.unlocked` becomes true.
Rectangle {
    id: root

    property var account

    /// True when no vault exists yet. The account service re-reads the file on
    /// construction, so this is stable for the lifetime of the window.
    readonly property bool createMode: root.account ? !root.account.hasAccount : true

    color: Style.windowBackground
    bottomLeftRadius: Style.windowRadius
    bottomRightRadius: Style.windowRadius

    /// Last account name typed, kept in plain QSettings (never the password).
    Settings {
        id: preferences
        category: "account"
        property string username: ""
    }

    Component.onCompleted: {
        usernameField.text = preferences.username
        if (root.createMode) {
            usernameField.forceActiveFocus()
        } else {
            passwordField.forceActiveFocus()
        }
    }

    function submit() {
        if (!root.account) {
            return
        }

        const username = usernameField.text.trim()
        preferences.username = username

        if (root.createMode) {
            root.account.createAccount(username, passwordField.text,
                                       confirmField.text)
        } else {
            root.account.unlock(username, passwordField.text)
        }
    }

    /// Local validation errors and vault errors share one line.
    readonly property string visibleError: root.account ? root.account.lastError : ""

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(root.width - 4 * Style.pad, 380)
        spacing: Style.gapLarge

        Image {
            Layout.alignment: Qt.AlignHCenter
            source: "qrc:/qt/qml/Pulse/Client/resources/icons/app.svg"
            sourceSize.width: 64
            sourceSize.height: 64
            fillMode: Image.PreserveAspectFit
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: Style.gapSmall

            Text {
                Layout.alignment: Qt.AlignHCenter
                text: qsTr("pulseTrader")
                color: Style.text
                font.pixelSize: Style.titleSize
                font.weight: Font.DemiBold
            }

            Text {
                Layout.alignment: Qt.AlignHCenter
                text: root.createMode
                      ? qsTr("Create a local account")
                      : qsTr("Unlock your local account")
                color: Style.secondaryText
                font.pixelSize: Style.fontSize
            }
        }

        // -- Credential form ------------------------------------------------
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: form.implicitHeight + 2 * Style.pad
            radius: Style.radiusLarge
            color: Style.contentBackground
            border.width: 1
            border.color: Style.controlBorder

            ColumnLayout {
                id: form
                anchors.fill: parent
                anchors.margins: Style.pad
                spacing: Style.gap

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Style.gapSmall

                    Text {
                        text: qsTr("Username")
                        color: Style.secondaryText
                        font.pixelSize: Style.fontSizeSmall
                    }

                    MacTextField {
                        id: usernameField
                        Layout.fillWidth: true
                        placeholderText: qsTr("local account name")
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Style.gapSmall

                    Text {
                        text: qsTr("Password")
                        color: Style.secondaryText
                        font.pixelSize: Style.fontSizeSmall
                    }

                    MacTextField {
                        id: passwordField
                        Layout.fillWidth: true
                        echoMode: TextInput.Password
                        placeholderText: root.createMode
                                         ? qsTr("at least 8 characters")
                                         : qsTr("vault password")
                        onAccepted: root.submit()
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Style.gapSmall
                    visible: root.createMode

                    Text {
                        text: qsTr("Confirm password")
                        color: Style.secondaryText
                        font.pixelSize: Style.fontSizeSmall
                    }

                    MacTextField {
                        id: confirmField
                        Layout.fillWidth: true
                        echoMode: TextInput.Password
                        placeholderText: qsTr("repeat the password")
                        onAccepted: root.submit()
                    }
                }

                MacButton {
                    Layout.fillWidth: true
                    primary: true
                    text: root.createMode ? qsTr("Create account") : qsTr("Unlock")
                    onClicked: root.submit()
                }
            }
        }

        Text {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignHCenter
            visible: root.visibleError.length > 0
            text: root.visibleError
            color: Style.danger
            font.pixelSize: Style.fontSizeSmall
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
        }

        Text {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignHCenter
            text: root.createMode
                  ? qsTr("The password encrypts the local vault that stores "
                         + "your exchange keys. It is never stored; if you "
                         + "forget it, the vault cannot be recovered.")
                  : qsTr("Your exchange keys are decrypted only while this "
                         + "window is unlocked.")
            color: Style.tertiaryText
            font.pixelSize: Style.fontSizeSmall
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
        }
    }
}
