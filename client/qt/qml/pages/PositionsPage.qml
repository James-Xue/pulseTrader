import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Pulse.Client

/// Open positions, one row per engine position.
///
/// The engine reports positions for three markets; the filter keeps a spot
/// position from being read as a futures one. "All" is the previous view and
/// remains the default so nothing is hidden by surprise.
ColumnLayout {
    id: root

    property var controller

    /// 0 = all, 1 = spot, 2 = cfd, 3 = futures.
    property int marketIndex: 0

    readonly property var filteredModels: root.controller
        ? [root.controller.positionsModel,
           root.controller.spotPositions,
           root.controller.cfdPositions,
           root.controller.futuresPositions]
        : []

    readonly property var activeModel: root.filteredModels.length > root.marketIndex
                                       ? root.filteredModels[root.marketIndex] : null
    readonly property int count: root.activeModel ? root.activeModel.count : 0

    spacing: 0

    RowLayout {
        Layout.fillWidth: true
        Layout.margins: Style.pad
        spacing: Style.gap

        Text {
            text: qsTr("Positions: %1").arg(root.count)
            color: Style.text
            font.pixelSize: Style.fontSize
        }

        Item { Layout.fillWidth: true }

        MacSegmentedControl {
            Layout.preferredWidth: 360
            items: [
                qsTr("All (%1)").arg(root.controller ? root.controller.positionsModel.count : 0),
                qsTr("Spot (%1)").arg(root.controller ? root.controller.spotPositions.count : 0),
                qsTr("CFD (%1)").arg(root.controller ? root.controller.cfdPositions.count : 0),
                qsTr("Futures (%1)").arg(root.controller ? root.controller.futuresPositions.count : 0)
            ]
            currentIndex: root.marketIndex
            onActivated: function (index) { root.marketIndex = index }
        }

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
        model: root.activeModel

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
                    elide: Text.ElideRight
                }
                Text {
                    text: model.side === undefined ? "" : model.side
                    // The engine serializes lowercase "buy" / "sell".
                    color: "buy" === model.side ? Style.success : Style.danger
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