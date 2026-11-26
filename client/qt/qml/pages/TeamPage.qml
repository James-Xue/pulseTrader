import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Pulse.Client

/// External team view.
///
/// PT is the execution hand: the GT runtime places orders with
/// client_order_id `xau_team:<member>:<seq>`, and the engine records that as
/// the position's strategy_id. This page groups the engine's positions and
/// trade history by that prefix, and seeds the enabled members from the GT
/// runner's team JSON so a trader with no fills yet is visible.
///
/// The table is responsive: every column is a Layout.fillWidth item with a
/// relative weight and minimumWidth 0, so the whole roster fits the window at
/// any width (long cells elide). A fixed-width table overflowed the content
/// area and clipped the right-hand columns with no visible scrollbar.
ColumnLayout {
    id: root

    property var controller

    readonly property var rows: root.controller ? root.controller.teamRows : []
    readonly property var summary: root.controller ? root.controller.teamSummary : ({})
    /// Relative column weights, shared by the header and every row.
    readonly property var weights: [1.8, 1.1, 1.1, 0.7, 1.4, 1.2, 1.0, 2.0, 2.6]

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

    // Positions poll at 500 ms, but Entries / Last entry come from the trade
    // history, which is fetched on demand. Without this timer a fill that
    // happens while the page is open stays invisible until Refresh is clicked.
    Timer {
        interval: 10000
        running: true
        repeat: true
        onTriggered: if (root.controller) root.controller.fetchTradeHistory()
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

    // Two rows: the account line and the exposure line. One row clipped the
    // right-hand values on narrower windows.
    Rectangle {
        Layout.fillWidth: true
        Layout.leftMargin: Style.pad
        Layout.rightMargin: Style.pad
        implicitHeight: summaryColumn.implicitHeight + 2 * Style.pad
        radius: Style.radius
        color: Style.contentBackground

        ColumnLayout {
            id: summaryColumn
            anchors.fill: parent
            anchors.margins: Style.pad
            spacing: Style.gapSmall

            RowLayout {
                Layout.fillWidth: true
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
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Style.gap * 2

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
                Item { Layout.fillWidth: true }
            }
        }
    }

    // The list and the empty state share one box. The ListView owns the header
    // so the header scrolls with the rows.
    Item {
        Layout.fillWidth: true
        Layout.fillHeight: true

        ListView {
            id: list
            anchors.fill: parent
            anchors.margins: Style.pad
            clip: true
            spacing: 6
            model: root.rows

            header: RowLayout {
                width: list.width
                height: 24
                spacing: Style.gapSmall

                Text { text: qsTr("Member"); color: Style.secondaryText
                       font.pixelSize: 11
                       Layout.fillWidth: true
                       Layout.preferredWidth: root.weights[0]
                       Layout.minimumWidth: 0
                       elide: Text.ElideRight }
                Text { text: qsTr("Budget"); color: Style.secondaryText
                       font.pixelSize: 11
                       Layout.fillWidth: true
                       Layout.preferredWidth: root.weights[1]
                       Layout.minimumWidth: 0
                       elide: Text.ElideRight }
                Text { text: qsTr("Risk/trade"); color: Style.secondaryText
                       font.pixelSize: 11
                       Layout.fillWidth: true
                       Layout.preferredWidth: root.weights[2]
                       Layout.minimumWidth: 0
                       elide: Text.ElideRight }
                Text { text: qsTr("Open"); color: Style.secondaryText
                       font.pixelSize: 11
                       Layout.fillWidth: true
                       Layout.preferredWidth: root.weights[3]
                       Layout.minimumWidth: 0
                       elide: Text.ElideRight }
                Text { text: qsTr("Total notional"); color: Style.secondaryText
                       font.pixelSize: 11
                       Layout.fillWidth: true
                       Layout.preferredWidth: root.weights[4]
                       Layout.minimumWidth: 0
                       elide: Text.ElideRight }
                Text { text: qsTr("uPnL"); color: Style.secondaryText
                       font.pixelSize: 11
                       Layout.fillWidth: true
                       Layout.preferredWidth: root.weights[5]
                       Layout.minimumWidth: 0
                       elide: Text.ElideRight }
                Text { text: qsTr("Entries"); color: Style.secondaryText
                       font.pixelSize: 11
                       Layout.fillWidth: true
                       Layout.preferredWidth: root.weights[6]
                       Layout.minimumWidth: 0
                       elide: Text.ElideRight }
                Text { text: qsTr("Last entry"); color: Style.secondaryText
                       font.pixelSize: 11
                       Layout.fillWidth: true
                       Layout.preferredWidth: root.weights[7]
                       Layout.minimumWidth: 0
                       elide: Text.ElideRight }
                Text { text: qsTr("Symbols"); color: Style.secondaryText
                       font.pixelSize: 11
                       Layout.fillWidth: true
                       Layout.preferredWidth: root.weights[8]
                       Layout.minimumWidth: 0
                       elide: Text.ElideRight }
            }

            delegate: Rectangle {
                width: list.width
                height: 36
                radius: Style.radius
                color: Style.contentBackground

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    spacing: Style.gapSmall

                    Text {
                        text: modelData.member || "—"
                        color: Style.text
                        font.pixelSize: 12
                        Layout.fillWidth: true
                        Layout.preferredWidth: root.weights[0]
                        Layout.minimumWidth: 0
                        elide: Text.ElideRight

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                memberDialog.member = modelData
                                memberDialog.mode = 0
                                memberDialog.open()
                            }
                        }
                    }
                    Text {
                        text: Number(modelData.budget || 0).toFixed(2)
                        color: Style.secondaryText
                        font.pixelSize: 12
                        Layout.fillWidth: true
                        Layout.preferredWidth: root.weights[1]
                        Layout.minimumWidth: 0
                        elide: Text.ElideRight
                    }
                    Text {
                        text: Number(modelData.risk_per_trade || 0).toFixed(2)
                        color: Style.secondaryText
                        font.pixelSize: 12
                        Layout.fillWidth: true
                        Layout.preferredWidth: root.weights[2]
                        Layout.minimumWidth: 0
                        elide: Text.ElideRight
                    }
                    Text {
                        text: String(modelData.open_positions || 0)
                        color: Style.text
                        font.pixelSize: 12
                        Layout.fillWidth: true
                        Layout.preferredWidth: root.weights[3]
                        Layout.minimumWidth: 0
                        elide: Text.ElideRight
                    }
                    Text {
                        text: Number(modelData.notional || 0).toFixed(2)
                        color: Style.text
                        font.pixelSize: 12
                        Layout.fillWidth: true
                        Layout.preferredWidth: root.weights[4]
                        Layout.minimumWidth: 0
                        elide: Text.ElideRight
                    }
                    Text {
                        text: Number(modelData.unrealized_pnl || 0).toFixed(2)
                        color: Number(modelData.unrealized_pnl || 0) >= 0
                               ? Style.danger : Style.success
                        font.pixelSize: 12
                        Layout.fillWidth: true
                        Layout.preferredWidth: root.weights[5]
                        Layout.minimumWidth: 0
                        elide: Text.ElideRight
                    }
                    Text {
                        text: String(modelData.entries || 0)
                        color: Style.text
                        font.pixelSize: 12
                        Layout.fillWidth: true
                        Layout.preferredWidth: root.weights[6]
                        Layout.minimumWidth: 0
                        elide: Text.ElideRight
                    }
                    Text {
                        text: modelData.last_entry_ms
                              ? new Date(Number(modelData.last_entry_ms))
                                    .toLocaleString(Qt.locale(), "MM-dd HH:mm:ss")
                              : "—"
                        color: Style.secondaryText
                        font.pixelSize: 12
                        Layout.fillWidth: true
                        Layout.preferredWidth: root.weights[7]
                        Layout.minimumWidth: 0
                        elide: Text.ElideRight
                    }
                    Text {
                        text: modelData.symbols || "—"
                        color: Style.secondaryText
                        font.pixelSize: 11
                        Layout.fillWidth: true
                        Layout.preferredWidth: root.weights[8]
                        Layout.minimumWidth: 0
                        elide: Text.ElideRight
                    }
                }
            }
        }

        EmptyState {
            anchors.centerIn: parent
            visible: list.count === 0
            text: qsTr("No team traders — attributed xau_team orders appear here")
        }
    }

    // Skill-card details: click a member name to open it.
    TeamMemberDialog {
        id: memberDialog
    }
}
