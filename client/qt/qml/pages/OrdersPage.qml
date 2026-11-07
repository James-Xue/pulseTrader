import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Pulse.Client

/// Active orders and recent execution reports.
///
/// The two payloads carry different timestamp units (milliseconds for active
/// orders, nanoseconds for reports), so each tab renders its own `*_str`
/// field instead of formatting the raw number.
ColumnLayout {
    id: root

    property var controller

    spacing: 0

    TabBar {
        id: tabs
        Layout.fillWidth: true
        TabButton { text: qsTr("Active (%1)").arg(root.activeCount) }
        TabButton { text: qsTr("Recent (%1)").arg(root.reportCount) }
    }

    readonly property int activeCount:
        root.controller ? root.controller.ordersModel.rowCount() : 0
    readonly property int reportCount:
        root.controller ? root.controller.reportsModel.rowCount() : 0

    StackLayout {
        Layout.fillWidth: true
        Layout.fillHeight: true
        currentIndex: tabs.currentIndex

        ListView {
            id: activeList
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: Style.pad
            spacing: 6
            clip: true
            model: root.controller ? root.controller.ordersModel : null

            delegate: Rectangle {
                width: activeList.width
                height: 42
                radius: Style.radius
                color: Style.surface

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    spacing: Style.gap

                    Text {
                        text: model.symbol === undefined ? "" : model.symbol
                        color: Style.text
                        font.pixelSize: 12
                        Layout.preferredWidth: 130
                    }
                    Text {
                        text: model.side === undefined ? "" : model.side
                        color: model.side === "Buy" ? Style.good : Style.bad
                        font.pixelSize: 12
                        Layout.preferredWidth: 60
                    }
                    Text {
                        text: model.order_type === undefined ? "" : model.order_type
                        color: Style.muted
                        font.pixelSize: 12
                        Layout.preferredWidth: 80
                    }
                    Text {
                        text: model.price === undefined ? "" : String(model.price)
                        color: Style.text
                        font.pixelSize: 12
                        Layout.preferredWidth: 100
                    }
                    Text {
                        text: model.status === undefined ? "" : model.status
                        color: Style.muted
                        font.pixelSize: 11
                        Layout.fillWidth: true
                    }
                }
            }

            EmptyState {
                visible: activeList.count === 0
                text: qsTr("No active orders")
            }
        }

        ListView {
            id: reportList
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: Style.pad
            spacing: 6
            clip: true
            model: root.controller ? root.controller.reportsModel : null

            delegate: Rectangle {
                width: reportList.width
                height: 42
                radius: Style.radius
                color: Style.surface

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    spacing: Style.gap

                    Text {
                        text: model.symbol === undefined ? "" : model.symbol
                        color: Style.text
                        font.pixelSize: 12
                        Layout.preferredWidth: 130
                    }
                    Text {
                        text: model.status === undefined ? "" : model.status
                        color: Style.muted
                        font.pixelSize: 12
                        Layout.preferredWidth: 90
                    }
                    Text {
                        text: model.fill_price === undefined ? "" : String(model.fill_price)
                        color: Style.text
                        font.pixelSize: 12
                        Layout.preferredWidth: 110
                    }
                    Text {
                        text: model.fill_time_str === undefined ? ""
                              : model.fill_time_str
                        color: Style.muted
                        font.pixelSize: 11
                        Layout.fillWidth: true
                    }
                }
            }

            EmptyState {
                visible: reportList.count === 0
                text: qsTr("No recent fills")
            }
        }
    }
}
