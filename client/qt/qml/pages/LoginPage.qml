import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtCore
import Pulse.Client

/// Full-window login view for the L9 control socket.
///
/// Shown instead of the main shell until the controller reports Ready. The
/// credential is the shared control-plane token (not an exchange API key):
///
/// 1. The engine operator sets `[control] token` in trading.toml. When it is
///    empty, authentication is disabled and the token field may stay blank.
/// 2. The token is sent through the `auth` JSON-RPC handshake that RpcClient
///    performs before the first poll.
/// 3. The form persists host/port/TLS unconditionally; the token is written
///    to QSettings only when "Remember" is checked, so a shared machine does
///    not keep the secret on disk by default.
Rectangle {
    id: root

    property var controller

    /// Set while a connection attempt is in flight. The fields go read-only
    /// and the primary button reports progress instead of accepting a second
    /// click that would race the first handshake.
    readonly property bool connecting: root.controller
                                       && root.controller.connectionState
                                          === EngineController.Connecting

    /// Validation message produced locally (bad port) — kept separate from
    /// the controller's lastError so a stale engine error does not mask it.
    property string localError: ""

    color: Style.windowBackground
    bottomLeftRadius: Style.windowRadius
    bottomRightRadius: Style.windowRadius

    /// Persisted connection preferences.
    ///
    /// `autoLogin` is separate from `remember` on purpose: remembering the
    /// host without remembering the token is useful, but reconnecting
    /// automatically without a saved token would silently land in whatever
    /// state the engine accepts.
    Settings {
        id: connection
        category: "connection"
        property string host: "127.0.0.1"
        property int port: 8081
        property bool tls: false
        property bool remember: true
        property string token: ""
        property bool autoLogin: false
    }

    Component.onCompleted: {
        hostField.text = connection.host
        portField.text = String(connection.port)
        tokenField.text = connection.token
        tlsBox.checked = connection.tls
        rememberBox.checked = connection.remember

        // Reconnect without showing the form only when the user asked for it
        // AND the saved token is available.
        if (connection.autoLogin && connection.remember) {
            attemptConnect()
        } else {
            hostField.forceActiveFocus()
        }
    }

    /// Validate the form, persist what the user chose, and ask the controller
    /// to open the three sockets. Kept in one function so the button, the
    /// Return key and the startup restore path cannot drift apart.
    function attemptConnect() {
        const host = hostField.text.trim()
        const port = parseInt(portField.text)

        // 1. Reject input the socket layer cannot represent.
        if (0 === host.length) {
            root.localError = qsTr("Enter a host name or address.")
            return
        }
        if (!(port > 0 && port < 65536)) {
            root.localError = qsTr("Enter a port between 1 and 65535.")
            return
        }
        root.localError = ""

        // 2. Host/port/TLS are always remembered; the token only on request.
        connection.host = host
        connection.port = port
        connection.tls = tlsBox.checked
        connection.remember = rememberBox.checked
        if (rememberBox.checked) {
            connection.token = tokenField.text
            connection.autoLogin = true
        } else {
            connection.token = ""
            connection.autoLogin = false
        }

        // 3. Hand off to the controller; Main.qml switches to the shell when
        //    the aggregate state becomes Ready.
        if (root.controller) {
            root.controller.connectToEngine(host, port, tlsBox.checked,
                                            tokenField.text)
        }
    }

    /// Error line. A local validation message wins; otherwise the controller's
    /// last error is shown only while disconnected, so a stale message from a
    /// previous session does not flash during a new handshake.
    readonly property string visibleError: root.localError.length > 0
        ? root.localError
        : (root.controller
           && root.controller.connectionState === EngineController.Disconnected
           ? root.controller.lastError : "")

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
                text: qsTr("Connect to the engine")
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

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Style.gap

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: Style.gapSmall

                        Text {
                            text: qsTr("Host")
                            color: Style.secondaryText
                            font.pixelSize: Style.fontSizeSmall
                        }

                        MacTextField {
                            id: hostField
                            Layout.fillWidth: true
                            enabled: !root.connecting
                            placeholderText: qsTr("127.0.0.1")
                        }
                    }

                    ColumnLayout {
                        Layout.preferredWidth: 96
                        spacing: Style.gapSmall

                        Text {
                            text: qsTr("Port")
                            color: Style.secondaryText
                            font.pixelSize: Style.fontSizeSmall
                        }

                        MacTextField {
                            id: portField
                            Layout.fillWidth: true
                            enabled: !root.connecting
                            inputMethodHints: Qt.ImhDigitsOnly
                            placeholderText: qsTr("8081")
                        }
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Style.gapSmall

                    Text {
                        text: qsTr("Token")
                        color: Style.secondaryText
                        font.pixelSize: Style.fontSizeSmall
                    }

                    MacTextField {
                        id: tokenField
                        Layout.fillWidth: true
                        enabled: !root.connecting
                        echoMode: TextInput.Password
                        placeholderText: qsTr("control-plane token (blank if none)")
                        onAccepted: root.attemptConnect()
                    }
                }

                MacCheckBox {
                    id: tlsBox
                    enabled: !root.connecting
                    text: qsTr("Use TLS")
                }

                MacCheckBox {
                    id: rememberBox
                    enabled: !root.connecting
                    text: qsTr("Remember this connection")
                }

                MacButton {
                    Layout.fillWidth: true
                    primary: true
                    enabled: !root.connecting
                    text: root.connecting ? qsTr("Connecting…") : qsTr("Connect")
                    onClicked: root.attemptConnect()
                }
            }
        }

        // -- Error / hint ---------------------------------------------------
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
            text: qsTr("The token is set by the engine in [control] token. "
                       + "Leave it empty when the engine has no token.")
            color: Style.tertiaryText
            font.pixelSize: Style.fontSizeSmall
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
        }
    }
}
