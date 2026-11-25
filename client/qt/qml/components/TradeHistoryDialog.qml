import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Pulse.Client

/// Non-modal trade history, ported from UeHao's TradeHistoryDialog.
///
/// Column sizes are explicit and shared by the header (inside the ListView's
/// header slot, so it gets the same viewport width) and by every delegate;
/// numeric cells are right-aligned in a monospace face. Without that, a
/// ListView's vertical scrollbar shrinks the rows but not a header outside
/// the view, and the columns drift apart.
///
/// A-/A+ zoom the table between 80% and 180% without re-laying out the dialog.
Dialog {
    id: root

    property var controller
    property real zoom: 1.0

    readonly property real cellFont: Style.fontSizeSmall * root.zoom
    readonly property var columnWidths: [150, 110, 60, 80, 100, 90, 80, 80, 140]

    function colWidth(index) {
        return root.columnWidths[index] * root.zoom
    }

    title: qsTr("Trade History")
    modal: false
    width: 1080
    height: 560
    standardButtons: Dialog.NoButton
    onOpened: if (root.controller) root.controller.fetchTradeHistory()

    background: Rectangle {
        color: Style.contentBackground
        radius: Style.radius
        border.color: Style.separator
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Style.pad
        spacing: Style.gapSmall

        RowLayout {
            Layout.fillWidth: true
            spacing: Style.gapSmall

            Text {
                Layout.fillWidth: true
                color: Style.secondaryText
                font.pixelSize: Style.fontSizeSmall
                wrapMode: Text.WordWrap
                text: {
                    const s = root.controller ? root.controller.tradeHistorySummary : null
                    if (!s || !s.count)
                        return qsTr("No trades yet — fills appear here once recorded")
                    return qsTr("%1 trades · %2 filled · win rate %3% · net %4 · fees %5")
                        .arg(s.count).arg(s.filled)
                        .arg(((s.win_rate || 0) * 100).toFixed(1))
                        .arg(Number(s.net_pnl || 0).toFixed(2))
                        .arg(Number(s.fees || 0).toFixed(2))
                }
            }
        }

        ListView {
            id: historyList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: root.controller ? root.controller.tradeHistory : []

            header: RowLayout {
                width: ListView.view.width
                spacing: Style.gapSmall

                Text { text: qsTr("Time"); color: Style.tertiaryText
                       font.pixelSize: root.cellFont; Layout.preferredWidth: root.colWidth(0) }
                Text { text: qsTr("Symbol"); color: Style.tertiaryText
                       font.pixelSize: root.cellFont; Layout.preferredWidth: root.colWidth(1) }
                Text { text: qsTr("Side"); color: Style.tertiaryText
                       font.pixelSize: root.cellFont; Layout.preferredWidth: root.colWidth(2) }
                Text { text: qsTr("Qty"); color: Style.tertiaryText; horizontalAlignment: Text.AlignRight
                       font.pixelSize: root.cellFont; Layout.preferredWidth: root.colWidth(3) }
                Text { text: qsTr("Price"); color: Style.tertiaryText; horizontalAlignment: Text.AlignRight
                       font.pixelSize: root.cellFont; Layout.preferredWidth: root.colWidth(4) }
                Text { text: qsTr("PnL"); color: Style.tertiaryText; horizontalAlignment: Text.AlignRight
                       font.pixelSize: root.cellFont; Layout.preferredWidth: root.colWidth(5) }
                Text { text: qsTr("Fees"); color: Style.tertiaryText; horizontalAlignment: Text.AlignRight
                       font.pixelSize: root.cellFont; Layout.preferredWidth: root.colWidth(6) }
                Text { text: qsTr("Status"); color: Style.tertiaryText
                       font.pixelSize: root.cellFont; Layout.preferredWidth: root.colWidth(7) }
                Text { text: qsTr("Strategy"); color: Style.tertiaryText
                       font.pixelSize: root.cellFont; Layout.fillWidth: true }
            }

            delegate: RowLayout {
                width: ListView.view.width
                spacing: Style.gapSmall

                Text {
                    text: new Date(Number(modelData.time_ms) || 0)
                              .toLocaleString(Qt.locale(), "MM-dd HH:mm:ss")
                    color: Style.text; font.pixelSize: root.cellFont
                    font.family: "monospace"
                    Layout.preferredWidth: root.colWidth(0)
                }
                Text {
                    text: modelData.symbol
                    color: Style.text; font.pixelSize: root.cellFont
                    Layout.preferredWidth: root.colWidth(1)
                    elide: Text.ElideRight
                }
                Text {
                    text: modelData.side
                    color: modelData.side === "buy" ? Style.success : Style.danger
                    font.pixelSize: root.cellFont
                    Layout.preferredWidth: root.colWidth(2)
                }
                Text {
                    text: Number(modelData.quantity || 0).toFixed(4)
                    color: Style.text; font.pixelSize: root.cellFont
                    font.family: "monospace"
                    horizontalAlignment: Text.AlignRight
                    Layout.preferredWidth: root.colWidth(3)
                }
                Text {
                    text: Number(modelData.price || 0).toFixed(2)
                    color: Style.text; font.pixelSize: root.cellFont
                    font.family: "monospace"
                    horizontalAlignment: Text.AlignRight
                    Layout.preferredWidth: root.colWidth(4)
                }
                Text {
                    text: Number(modelData.pnl || 0).toFixed(2)
                    color: Number(modelData.pnl || 0) >= 0 ? Style.success : Style.danger
                    font.pixelSize: root.cellFont
                    font.family: "monospace"
                    horizontalAlignment: Text.AlignRight
                    Layout.preferredWidth: root.colWidth(5)
                }
                Text {
                    text: Number(modelData.fees || 0).toFixed(2)
                    color: Style.secondaryText; font.pixelSize: root.cellFont
                    font.family: "monospace"
                    horizontalAlignment: Text.AlignRight
                    Layout.preferredWidth: root.colWidth(6)
                }
                Text {
                    text: modelData.status
                    color: Style.secondaryText; font.pixelSize: root.cellFont
                    Layout.preferredWidth: root.colWidth(7)
                }
                Text {
                    text: {
                        // Team fills carry xau_team:<member>:<seq>; show the
                        // member. Otherwise fall back to the recorded strategy.
                        const cid = String(modelData.client_order_id || "")
                        if (cid.indexOf("xau_team:") === 0) {
                            const parts = cid.split(":")
                            if (parts.length > 1 && parts[1] !== "") {
                                return parts[1]
                            }
                        }
                        return modelData.strategy || cid || "—"
                    }
                    color: Style.tertiaryText; font.pixelSize: root.cellFont
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
            }
        }

        EmptyState {
            visible: historyList.count === 0
            text: qsTr("No trades yet")
        }

        RowLayout {
            Layout.fillWidth: true

            // Zoom controls live here so they are impossible to miss.
            MacButton {
                text: "A−"
                onClicked: root.zoom = Math.max(0.8, root.zoom - 0.1)
            }
            Text {
                text: Math.round(root.zoom * 100) + "%"
                color: Style.secondaryText
                font.pixelSize: Style.fontSizeSmall
                Layout.preferredWidth: 44
                horizontalAlignment: Text.AlignHCenter
            }
            MacButton {
                text: "A+"
                onClicked: root.zoom = Math.min(1.8, root.zoom + 0.1)
            }

            Item { Layout.fillWidth: true }
            MacButton {
                text: qsTr("Refresh")
                onClicked: if (root.controller) root.controller.fetchTradeHistory()
            }
            MacButton {
                text: qsTr("Close")
                onClicked: root.close()
            }
        }
    }
}
