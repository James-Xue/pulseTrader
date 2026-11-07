import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Pulse.Client

/// Open positions, one row per engine position.
ColumnLayout {
    id: root

    property var controller

    spacing: 0

    RowLayout {
        Layout.fillWidth: true
        Layout.margins: Style.pad
        spacing: Style.gap

        Text {
            text: qsTr("Positions: %1").arg(
                      root.controller ? root.controller.positionsModel.rowCount() : 0)
            color: Style.text
            font.pixelSize: Style.fontSize
        }

        Item { Layout.fillWidth: true }

        MacButton {
            text: qsTr("Refresh")
            onClicked: if (root.controller) root.controller.refreshAll()
        }
    }

    HeaderRow {
        Layout.fillWidth: true
        Layout.leftMargin: Style.pad
        Layout.rightMargin: Style.pad
    }

    ListView {
        id: list
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.margins: Style.pad
        spacing: 6
        clip: true
        model: root.controller ? root.controller.positionsModel : null

        delegate: Rectangle {
            width: list.width
            height: 44
            radius: Style.radius
            color: Style.contentBackground

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                spacing: Style.gap

                Text {
                    text: model.symbol === undefined ? "" : model.symbol
                    color: Style.text
                    font.pixelSize: 12
                    Layout.preferredWidth: 140
                }
                Text {
                    text: model.side === undefined ? "" : model.side
                    color: model.side === "Buy" ? Style.success : Style.danger
                    font.pixelSize: 12
                    Layout.preferredWidth: 60
                }
                Text {
                    text: model.quantity === undefined ? "" : String(model.quantity)
                    color: Style.text
                    font.pixelSize: 12
                    Layout.preferredWidth: 90
                }
                Text {
                    text: model.entry_price === undefined ? "" : String(model.entry_price)
                    color: Style.text
                    font.pixelSize: 12
                    Layout.preferredWidth: 110
                }
                Text {
                    text: model.unrealized_pnl === undefined ? ""
                          : Number(model.unrealized_pnl).toFixed(2)
                    color: Number(model.unrealized_pnl) >= 0 ? Style.success : Style.danger
                    font.pixelSize: 12
                    Layout.preferredWidth: 100
                }
                Text {
                    text: model.market_type === undefined ? "" : model.market_type
                    color: Style.secondaryText
                    font.pixelSize: 11
                    Layout.fillWidth: true
                }
            }
        }

        EmptyState {
            visible: list.count === 0
            text: qsTr("No open positions")
        }
    }

    component HeaderRow: RowLayout {
        spacing: Style.gap

        Text { text: qsTr("Symbol"); color: Style.secondaryText; font.pixelSize: 11; Layout.preferredWidth: 140 }
        Text { text: qsTr("Side"); color: Style.secondaryText; font.pixelSize: 11; Layout.preferredWidth: 60 }
        Text { text: qsTr("Qty"); color: Style.secondaryText; font.pixelSize: 11; Layout.preferredWidth: 90 }
        Text { text: qsTr("Entry"); color: Style.secondaryText; font.pixelSize: 11; Layout.preferredWidth: 110 }
        Text { text: qsTr("uPnL"); color: Style.secondaryText; font.pixelSize: 11; Layout.preferredWidth: 100 }
        Text { text: qsTr("Market"); color: Style.secondaryText; font.pixelSize: 11; Layout.fillWidth: true }
    }
}
