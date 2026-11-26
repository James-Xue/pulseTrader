import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Pulse.Client

/// External team view.
///
/// PT is the execution hand: the GT runtime places orders with
/// client_order_id `xau_team:<member>:<seq>`, and the engine records that as
/// the position's strategy_id. This page groups the engine's positions and
/// trade history by that prefix, so the traders are visible without opening
/// GT's own desktop app. Rows without the prefix (built-in strategies, venue
/// exits) are deliberately absent.
ColumnLayout {
    id: root

    property var controller

    readonly property var rows: root.controller ? root.controller.teamRows : []
    readonly property var summary: root.controller ? root.controller.teamSummary : ({})

    spacing: 0

    Component.onCompleted: if (root.controller) root.controller.fetchTradeHistory()

    // The page is built before the REST socket is Ready, so the first fetch is
    // dropped. Retry when the connection comes up (the controller de-dupes).
    Connections {
        target: root.controller
        function onConnectionStateChanged() {
            if (root.controller) root.controller.fetchTradeHistory()
        }
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.margins: Style.pad
        spacing: Style.gap

        Text {
            text: qsTr("Team")
            color: Style.text
            font.pixelSize: Style.titleSize
            font.weight: Font.DemiBold
        }
        Text {
            text: qsTr("%1 member(s) · %2 open · %3 entries")
                  .arg(Number(root.summary.members || 0))
                  .arg(Number(root.summary.open_positions || 0))
                  .arg(Number(root.summary.entries || 0))
            color: Style.secondaryText
            font.pixelSize: Style.fontSizeSmall
        }
        Item { Layout.fillWidth: true }
        MacButton {
            text: qsTr("Refresh")
            onClicked: {
                if (!root.controller) return
                root.controller.refreshAll()
                root.controller.fetchTradeHistory()
            }
        }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.leftMargin: Style.pad
        Layout.rightMargin: Style.pad
        implicitHeight: summaryRow.implicitHeight + 2 * Style.pad
        radius: Style.radius
        color: Style.contentBackground

        RowLayout {
            id: summaryRow
            anchors.fill: parent
            anchors.margins: Style.pad
            spacing: Style.gap * 2

            Text {
                text: qsTr("Mode") + ": " + (root.summary.mode || "—")
                color: Style.secondaryText
                font.pixelSize: Style.fontSizeSmall
            }
            Text {
                text: qsTr("Equity") + ": "
                      + Number(root.summary.equity || 0).toFixed(2)
                color: Style.text
                font.pixelSize: Style.fontSizeSmall
            }
            Text {
                text: qsTr("Cash") + ": "
                      + Number(root.summary.cash || 0).toFixed(2)
                color: Style.text
                font.pixelSize: Style.fontSizeSmall
            }
            Text {
                text: qsTr("Fees") + ": "
                      + Number(root.summary.fees || 0).toFixed(2)
                color: Style.secondaryText
                font.pixelSize: Style.fontSizeSmall
            }
            Item { Layout.fillWidth: true }
            Text {
                text: qsTr("Total notional") + ": "
                      + Number(root.summary.notional || 0).toFixed(2)
                color: Style.text
                font.pixelSize: Style.fontSizeSmall
            }
            Text {
                text: qsTr("uPnL") + ": "
                      + Number(root.summary.unrealized_pnl || 0).toFixed(2)
                color: Number(root.summary.unrealized_pnl || 0) >= 0
                       ? Style.danger : Style.success
                font.pixelSize: Style.fontSizeSmall
            }
        }
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.leftMargin: Style.pad + 10
        Layout.rightMargin: Style.pad + 10
        spacing: Style.gap

        Text { text: qsTr("Member"); color: Style.secondaryText
               font.pixelSize: 11; Layout.preferredWidth: 160 }
        Text { text: qsTr("Open"); color: Style.secondaryText
               font.pixelSize: 11; Layout.preferredWidth: 70 }
        Text { text: qsTr("Total notional"); color: Style.secondaryText
               font.pixelSize: 11; Layout.preferredWidth: 130 }
        Text { text: qsTr("uPnL"); color: Style.secondaryText
               font.pixelSize: 11; Layout.preferredWidth: 110 }
        Text { text: qsTr("Entries"); color: Style.secondaryText
               font.pixelSize: 11; Layout.preferredWidth: 90 }
        Text { text: qsTr("Last entry"); color: Style.secondaryText
               font.pixelSize: 11; Layout.preferredWidth: 170 }
        Text { text: qsTr("Symbols"); color: Style.secondaryText
               font.pixelSize: 11; Layout.fillWidth: true }
    }

    ListView {
        id: list
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.margins: Style.pad
        spacing: 6
        clip: true
        model: root.rows

        delegate: Rectangle {
            width: list.width
            height: 36
            radius: Style.radius
            color: Style.contentBackground

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                spacing: Style.gap

                Text {
                    text: modelData.member || "—"
                    color: Style.text
                    font.pixelSize: 12
                    Layout.preferredWidth: 160
                    elide: Text.ElideRight
                }
                Text {
                    text: String(modelData.open_positions || 0)
                    color: Style.text
                    font.pixelSize: 12
                    Layout.preferredWidth: 70
                }
                Text {
                    text: Number(modelData.notional || 0).toFixed(2)
                    color: Style.text
                    font.pixelSize: 12
                    Layout.preferredWidth: 130
                }
                Text {
                    text: Number(modelData.unrealized_pnl || 0).toFixed(2)
                    color: Number(modelData.unrealized_pnl || 0) >= 0
                           ? Style.danger : Style.success
                    font.pixelSize: 12
                    Layout.preferredWidth: 110
                }
                Text {
                    text: String(modelData.entries || 0)
                    color: Style.text
                    font.pixelSize: 12
                    Layout.preferredWidth: 90
                }
                Text {
                    text: modelData.last_entry_ms
                          ? new Date(Number(modelData.last_entry_ms))
                                .toLocaleString(Qt.locale(), "MM-dd HH:mm:ss")
                          : "—"
                    color: Style.secondaryText
                    font.pixelSize: 12
                    Layout.preferredWidth: 170
                }
                Text {
                    text: modelData.symbols || "—"
                    color: Style.secondaryText
                    font.pixelSize: 11
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                }
            }
        }

        EmptyState {
            visible: list.count === 0
            text: qsTr("No team traders — attributed xau_team orders appear here")
        }
    }
}
