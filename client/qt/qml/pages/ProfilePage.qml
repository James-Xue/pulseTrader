import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Pulse.Client

/// Exchange profile screen, shown after the local account is unlocked.
///
/// The exchange is fixed to Gate.io (the engine targets one exchange by
/// design); the profile selects the network and the market whose account the
/// engine should expose. Connect() persists the profile to the encrypted
/// vault, spawns the engine and attaches the RPC clients to its control
/// socket. The engine always runs with signal_only=true.
Rectangle {
    id: root

    property var account
    property var launcher
    property var engine

    /// Control socket port. The launcher owns the process and the port, so a
    /// single value here is enough.
    property int controlPort: 8081

    color: Style.windowBackground
    bottomLeftRadius: Style.windowRadius
    bottomRightRadius: Style.windowRadius

    // -- Selections ---------------------------------------------------------
    property int networkIndex: 0          // 0 = mainnet, 1 = testnet
    property int marketIndex: 0           // 0 = spot, 1 = futures, 2 = cfd

    readonly property bool testnet: root.networkIndex === 1
    readonly property var marketNames: ["spot", "futures", "cfd"]
    readonly property string network: root.testnet ? "testnet" : "mainnet"

    /// Gate.io testnet is futures-only: the engine's validator rejects spot
    /// and cfd instances there, so the market is pinned while testnet is on.
    readonly property string market: root.testnet
                                     ? "futures"
                                     : root.marketNames[Math.max(0, root.marketIndex)]

    readonly property bool starting: root.launcher
                                     && root.launcher.running
                                     && root.engine
                                     && root.engine.connectionState !== EngineController.Ready
    readonly property bool busy: root.starting
                                 || (root.engine
                                     && root.engine.connectionState === EngineController.Connecting)

    /// Copy the decrypted profile into the form.
    ///
    /// This must run when the page becomes visible, not only at construction:
    /// StackLayout instantiates every child at startup, while the vault is
    /// still locked and the profile getters return empty strings. Reloading on
    /// unlock is what saves the user from retyping the API key every session.
    function reloadProfile() {
        if (!root.account) {
            return
        }

        root.networkIndex = root.account.profileNetwork === "testnet" ? 1 : 0
        const index = root.marketNames.indexOf(root.account.profileMarket)
        root.marketIndex = index >= 0 ? index : 0
        keyField.text = root.account.profileApiKey
        secretField.text = root.account.profileApiSecret
        proxyField.text = root.account.profileProxy
    }

    Component.onCompleted: root.reloadProfile()

    // Fired when the vault unlocks and this page replaces the login form.
    onVisibleChanged: {
        if (visible) {
            root.reloadProfile()
        }
    }

    // Fired when the profile is saved (and after unlock, in case visibility
    // changes in the same frame).
    Connections {
        target: root.account

        function onProfileChanged() {
            if (root.visible) {
                root.reloadProfile()
            }
        }
    }

    /// Persist the profile, start the engine, then attach the RPC clients.
    /// Each step reports its own failure through the shared error line.
    function connectNow() {
        if (!root.account || !root.launcher || !root.engine) {
            return
        }

        if (!root.account.saveProfile(root.network, root.market,
                                      keyField.text, secretField.text,
                                      proxyField.text)) {
            return
        }

        if (!root.launcher.start(root.account.profileApiKey,
                                 root.account.profileApiSecret,
                                 root.network, root.market,
                                 root.account.profileProxy,
                                 root.controlPort)) {
            return
        }

        // The engine needs a moment to bind; RpcClient retries with backoff
        // until the socket accepts, so connecting now is safe.
        root.engine.connectToEngine("127.0.0.1", root.controlPort, false, "")
    }

    /// Stop the engine and wipe the in-memory vault key.
    function signOut() {
        if (root.launcher) {
            root.launcher.stop()
        }
        if (root.engine) {
            root.engine.disconnectFromEngine()
        }
        if (root.account) {
            root.account.signOut()
        }
    }

    readonly property string visibleError: {
        if (root.account && root.account.lastError.length > 0) {
            return root.account.lastError
        }
        if (root.launcher && root.launcher.lastError.length > 0) {
            return root.launcher.lastError
        }
        if (root.engine
            && root.engine.connectionState === EngineController.Disconnected) {
            return root.engine.lastError
        }
        return ""
    }

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(root.width - 4 * Style.pad, 440)
        spacing: Style.gapLarge

        Text {
            Layout.alignment: Qt.AlignHCenter
            text: qsTr("Connect an exchange")
            color: Style.text
            font.pixelSize: Style.titleSize
            font.weight: Font.DemiBold
        }

        Text {
            Layout.alignment: Qt.AlignHCenter
            text: qsTr("Gate.io · read-only monitor")
            color: Style.secondaryText
            font.pixelSize: Style.fontSize
        }

        // -- Profile form ---------------------------------------------------
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

                    Text {
                        Layout.preferredWidth: 88
                        text: qsTr("Exchange")
                        color: Style.secondaryText
                        font.pixelSize: Style.fontSizeSmall
                    }

                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Gate.io")
                        color: Style.text
                        font.pixelSize: Style.fontSize
                        font.weight: Font.DemiBold
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Style.gap

                    Text {
                        Layout.preferredWidth: 88
                        text: qsTr("Network")
                        color: Style.secondaryText
                        font.pixelSize: Style.fontSizeSmall
                    }

                    MacSegmentedControl {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 26
                        items: [qsTr("Mainnet"), qsTr("Testnet")]
                        currentIndex: root.networkIndex
                        enabled: !root.busy
                        onActivated: function (index) { root.networkIndex = index }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Style.gap

                    Text {
                        Layout.preferredWidth: 88
                        text: qsTr("Market")
                        color: Style.secondaryText
                        font.pixelSize: Style.fontSizeSmall
                    }

                    MacSegmentedControl {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 26
                        items: [qsTr("Spot"), qsTr("Futures"), qsTr("CFD")]
                        currentIndex: root.testnet ? 1 : root.marketIndex
                        enabled: !root.testnet && !root.busy
                        onActivated: function (index) { root.marketIndex = index }
                    }
                }

                Text {
                    Layout.leftMargin: 100
                    visible: root.testnet
                    text: qsTr("Gate.io testnet is futures-only.")
                    color: Style.tertiaryText
                    font.pixelSize: Style.fontSizeSmall
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Style.gapSmall

                    Text {
                        text: qsTr("API key")
                        color: Style.secondaryText
                        font.pixelSize: Style.fontSizeSmall
                    }

                    MacTextField {
                        id: keyField
                        Layout.fillWidth: true
                        enabled: !root.busy
                        // The key identifies the account rather than
                        // authorising it, but it is still masked by default so
                        // a screenshot or a passer-by cannot read it. The
                        // reveal switch below covers both fields.
                        echoMode: revealBox.checked ? TextInput.Normal
                                                    : TextInput.Password
                        placeholderText: qsTr("Gate.io API key")
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Style.gapSmall

                    Text {
                        text: qsTr("API secret")
                        color: Style.secondaryText
                        font.pixelSize: Style.fontSizeSmall
                    }

                    MacTextField {
                        id: secretField
                        Layout.fillWidth: true
                        enabled: !root.busy
                        echoMode: revealBox.checked ? TextInput.Normal
                                                    : TextInput.Password
                        placeholderText: qsTr("Gate.io API secret")
                        onAccepted: root.connectNow()
                    }
                }

                MacCheckBox {
                    id: revealBox
                    enabled: !root.busy
                    text: qsTr("Show API key and secret")
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Style.gapSmall

                    Text {
                        text: qsTr("Proxy (optional)")
                        color: Style.secondaryText
                        font.pixelSize: Style.fontSizeSmall
                    }

                    MacTextField {
                        id: proxyField
                        Layout.fillWidth: true
                        enabled: !root.busy
                        placeholderText: qsTr("http://127.0.0.1:7897")
                    }
                }

                MacButton {
                    Layout.fillWidth: true
                    primary: true
                    enabled: !root.busy
                    text: root.starting
                          ? qsTr("Starting engine…")
                          : (root.engine && root.engine.connectionState === EngineController.Connecting
                             ? qsTr("Connecting…") : qsTr("Connect"))
                    onClicked: root.connectNow()
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

        RowLayout {
            Layout.fillWidth: true
            spacing: Style.gap

            Item { Layout.fillWidth: true }

            MacButton {
                text: qsTr("Sign out")
                onClicked: root.signOut()
            }
        }

        Text {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignHCenter
            text: qsTr("Keys are stored in an encrypted local vault (AES-256-GCM). "
                       + "The engine runs in signal-only mode and places no orders.")
            color: Style.tertiaryText
            font.pixelSize: Style.fontSizeSmall
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
        }
    }
}
