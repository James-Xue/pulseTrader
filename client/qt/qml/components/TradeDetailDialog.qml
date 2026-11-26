import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Pulse.Client

/// Read-only detail window for one trade-history row.
///
/// Opened by clicking a row in Orders → History. Shows every field the engine
/// returned, including the attribution ids; a team fill shows its member name
/// from `xau_team:<member>:<seq>`.
Dialog {
    id: root

    property var row

    title: qsTr("Trade Detail")
    modal: false
    width: 560
    height: 360
    standardButtons: Dialog.NoButton

    background: Rectangle {
        color: Style.contentBackground
        radius: Style.radius
        border.color: Style.separator
    }

    readonly property var fields: {
        const r = root.row || {}
        const cid = String(r.client_order_id || "")
        let member = "—"
        if (cid.indexOf("xau_team:") === 0) {
            const parts = cid.split(":")
            if (parts.length > 1) {
                member = parts[1]
            }
        }
        return [
            [qsTr("Time"), new Date(Number(r.time_ms) || 0)
                              .toLocaleString(Qt.locale(), "yyyy-MM-dd HH:mm:ss")],
            [qsTr("Symbol"), r.symbol || "—"],
            [qsTr("Side"), r.side || "—"],
            [qsTr("Market"), r.market_type || "—"],
            [qsTr("Quantity"), Number(r.quantity || 0).toFixed(4)],
            [qsTr("Price"), Number(r.price || 0).toFixed(2)],
            [qsTr("PnL"), Number(r.pnl || 0).toFixed(2)],
            [qsTr("Fees"), Number(r.fees || 0).toFixed(2)],
            [qsTr("Status"), r.status || "—"],
            [qsTr("Strategy"), r.strategy || "—"],
            [qsTr("Team member"), member],
            [qsTr("Order ID"), r.order_id || "—"],
            [qsTr("Client order ID"), cid || "—"],
        ]
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Style.pad
        spacing: Style.gapSmall

        GridLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            columns: 2
            columnSpacing: Style.gap
            rowSpacing: 4

            Repeater {
                model: root.fields
                SelectableText {
                    text: modelData[0]
                    color: Style.tertiaryText
                    font.pixelSize: Style.fontSizeSmall
                }
                SelectableText {
                    text: modelData[1]
                    color: Style.text
                    font.pixelSize: Style.fontSizeSmall
                    font.family: "monospace"
                    Layout.fillWidth: true
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            MacButton {
                text: qsTr("Close")
                onClicked: root.close()
            }
        }
    }
}
