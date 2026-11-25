import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Pulse.Client

/// Non-modal trade history, ported from UeHao's TradeHistoryDialog.
///
/// Same design: one summary line, one read-only table and Refresh / Close.
/// Rows come from the engine's `get_trade_history` (persisted trades.db), so
/// they survive restarts — unlike the Orders page's in-memory recentReports.
/// Opening the dialog triggers a fetch; Refresh re-fetches.
Dialog {
    id: root

    property var controller

    title: qsTr("Trade History")
    modal: false
    width: 980
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

        RowLayout {
            Layout.fillWidth: true
            spacing: Style.gapSmall
            Repeater {
                model: [qsTr("Time"), qsTr("Symbol"), qsTr("Side"), qsTr("Qty"),
                        qsTr("Price"), qsTr("PnL"), qsTr("Fees"), qsTr("Status"),
                        qsTr("Strategy")]
                Text {
                    text: modelData
                    color: Style.tertiaryText
                    font.pixelSize: Style.fontSizeSmall
                    Layout.fillWidth: true
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            height: Style.hairline
            color: Style.separator
        }

        ListView {
            id: historyList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: root.controller ? root.controller.tradeHistory : []
            delegate: RowLayout {
                width: ListView.view.width
                spacing: Style.gapSmall

                Text {
                    Layout.fillWidth: true
                    font.pixelSize: Style.fontSizeSmall
                    color: Style.text
                    text: new Date(Number(modelData.time_ms) || 0)
                              .toLocaleString(Qt.locale(), "MM-dd HH:mm:ss")
                }
                Text {
                    Layout.fillWidth: true
                    font.pixelSize: Style.fontSizeSmall
                    color: Style.text
                    text: modelData.symbol
                }
                Text {
                    Layout.fillWidth: true
                    font.pixelSize: Style.fontSizeSmall
                    color: modelData.side === "buy" ? Style.success : Style.danger
                    text: modelData.side
                }
                Text {
                    Layout.fillWidth: true
                    font.pixelSize: Style.fontSizeSmall
                    color: Style.text
                    text: Number(modelData.quantity || 0).toFixed(4)
                }
                Text {
                    Layout.fillWidth: true
                    font.pixelSize: Style.fontSizeSmall
                    color: Style.text
                    text: Number(modelData.price || 0).toFixed(2)
                }
                Text {
                    Layout.fillWidth: true
                    font.pixelSize: Style.fontSizeSmall
                    color: Number(modelData.pnl || 0) >= 0 ? Style.success : Style.danger
                    text: Number(modelData.pnl || 0).toFixed(2)
                }
                Text {
                    Layout.fillWidth: true
                    font.pixelSize: Style.fontSizeSmall
                    color: Style.secondaryText
                    text: Number(modelData.fees || 0).toFixed(2)
                }
                Text {
                    Layout.fillWidth: true
                    font.pixelSize: Style.fontSizeSmall
                    color: Style.secondaryText
                    text: modelData.status
                }
                Text {
                    Layout.fillWidth: true
                    font.pixelSize: Style.fontSizeSmall
                    color: Style.tertiaryText
                    elide: Text.ElideRight
                    text: modelData.strategy || modelData.client_order_id || "—"
                }
            }
        }

        EmptyState {
            visible: historyList.count === 0
            text: qsTr("No trades yet")
        }

        RowLayout {
            Layout.fillWidth: true
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
