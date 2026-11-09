import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
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
    property var dataServer

    spacing: Style.gap

    // The spot-holdings preference is owned by EngineController (persisted as
    // [dashboard] spotHoldings) so the Dashboard switch and this page stay in
    // sync without a second copy of the setting.

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

    // -- Data server --------------------------------------------------------
    //
    // The local market-data endpoint for downstream consumers. It listens on
    // loopback only, so this card is the only place the operator sees whether
    // a bot is actually attached and whether the engine feed is live.
    Rectangle {
        Layout.fillWidth: true
        Layout.leftMargin: Style.pad
        Layout.rightMargin: Style.pad
        Layout.preferredHeight: dataCard.implicitHeight + (Style.pad * 2)
        color: Style.controlBackground
        radius: Style.radius
        border.width: 1
        border.color: Style.separator

        ColumnLayout {
            id: dataCard
            anchors.fill: parent
            anchors.margins: Style.pad
            spacing: Style.gapSmall

            RowLayout {
                Layout.fillWidth: true
                spacing: Style.gapSmall

                Rectangle {
                    width: 8
                    height: 8
                    radius: 4
                    color: !root.dataServer
                           ? Style.tertiaryText
                           : (root.dataServer.listening ? Style.success
                                                        : Style.danger)
                }

                Text {
                    text: qsTr("Data server")
                    color: Style.text
                    font.pixelSize: Style.sectionTitleSize
                    font.weight: Font.DemiBold
                }

                Item { Layout.fillWidth: true }

                Text {
                    text: !root.dataServer
                          ? qsTr("unavailable")
                          : (root.dataServer.listening
                             ? qsTr("listening on 127.0.0.1:%1").arg(root.dataServer.port)
                             : qsTr("stopped"))
                    color: Style.secondaryText
                    font.pixelSize: Style.fontSizeSmall
                }
            }

            GridLayout {
                Layout.fillWidth: true
                columns: 4
                columnSpacing: Style.gapLarge
                rowSpacing: 2

                Text {
                    text: qsTr("Engine feed")
                    color: Style.secondaryText
                    font.pixelSize: Style.fontSizeSmall
                }
                Text {
                    text: root.dataServer && root.dataServer.engineReady
                          ? qsTr("ready") : qsTr("waiting")
                    color: root.dataServer && root.dataServer.engineReady
                           ? Style.success : Style.warning
                    font.pixelSize: Style.fontSizeSmall
                }
                Text {
                    text: qsTr("Consumers")
                    color: Style.secondaryText
                    font.pixelSize: Style.fontSizeSmall
                }
                Text {
                    text: root.dataServer
                          ? qsTr("%1 / %2").arg(root.dataServer.connectedCount)
                                          .arg(root.dataServer.maxClients)
                          : "—"
                    color: Style.text
                    font.pixelSize: Style.fontSizeSmall
                }
            }

            Text {
                Layout.fillWidth: true
                text: qsTr("XAUUSD · CFD · 1m · 500 candles · updates at 2 Hz")
                color: Style.tertiaryText
                font.pixelSize: Style.fontSizeSmall
                wrapMode: Text.WordWrap
            }

            // One row per attached consumer: the identity it presented in its
            // hello plus the last time any frame arrived from it.
            Column {
                Layout.fillWidth: true
                spacing: 2
                visible: root.dataServer && root.dataServer.connectedCount > 0

                Repeater {
                    model: root.dataServer ? root.dataServer.clients : []

                    delegate: Row {
                        required property var modelData
                        width: parent.width
                        spacing: Style.gap

                        Text {
                            width: 140
                            text: modelData.client_id
                            color: Style.text
                            font.pixelSize: Style.fontSizeSmall
                            elide: Text.ElideRight
                        }
                        Text {
                            width: 160
                            text: modelData.strategy.length > 0
                                  ? modelData.strategy : qsTr("(no strategy)")
                            color: Style.secondaryText
                            font.pixelSize: Style.fontSizeSmall
                            elide: Text.ElideRight
                        }
                        Text {
                            text: qsTr("last seen %1").arg(
                                      Qt.formatTime(new Date(modelData.last_seen_ms),
                                                    "hh:mm:ss"))
                            color: Style.tertiaryText
                            font.pixelSize: Style.fontSizeSmall
                        }
                    }
                }
            }

            Text {
                Layout.fillWidth: true
                visible: !root.dataServer || root.dataServer.connectedCount === 0
                text: qsTr("No consumers connected.")
                color: Style.tertiaryText
                font.pixelSize: Style.fontSizeSmall
            }
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
            checked: root.controller ? root.controller.spotHoldingsEnabled : false
            onToggled: {
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

    Connections {
        target: root.controller

        function onSpotHoldingsEnabledChanged() {
            holdingsSwitch.checked = root.controller.spotHoldingsEnabled
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
