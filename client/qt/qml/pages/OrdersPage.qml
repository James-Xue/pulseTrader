import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Pulse.Client

/// Active orders and recent execution reports.
///
/// Two views of the same subject, so they switch with a macOS segmented
/// control rather than a tab bar. The two payloads carry different timestamp
/// units (milliseconds for active orders, nanoseconds for reports), so each
/// view renders its own `*_str` field instead of formatting the raw number.
ColumnLayout {
    id: root

    property var controller

    spacing: 0

    readonly property int activeCount:
        root.controller ? root.controller.ordersModel.rowCount() : 0
    readonly property int reportCount:
        root.controller ? root.controller.reportsModel.rowCount() : 0

    /// Status colour shared by both views. Recognised terminal states get the
    /// semantic colour; anything else stays neutral rather than guessing.
    function statusColor(status) {
        if (status === "filled" || status === "closed") {
            return Style.success
        }
        if (status === "cancelled" || status === "canceled" || status === "rejected") {
            return Style.danger
        }
        if (status === "open" || status === "active" || status === "new") {
            return Style.accent
        }
        return Style.secondaryText
    }

    // -- Toolbar ------------------------------------------------------------
    RowLayout {
        Layout.fillWidth: true
        Layout.margins: Style.pad
        spacing: Style.gap

        Text {
            text: qsTr("Orders")
            color: Style.text
            font.pixelSize: Style.titleSize
            font.weight: Font.DemiBold
        }

        Text {
            text: qsTr("%1 active · %2 recent")
                  .arg(root.activeCount).arg(root.reportCount)
            color: Style.secondaryText
            font.pixelSize: Style.fontSizeSmall
        }

        Item { Layout.fillWidth: true }

        MacSegmentedControl {
            Layout.preferredWidth: 250
            items: [qsTr("Active (%1)").arg(root.activeCount),
                    qsTr("Recent (%1)").arg(root.reportCount)]
            currentIndex: viewStack.currentIndex
            onActivated: function (index) { viewStack.currentIndex = index }
        }

        MacButton {
            text: qsTr("Refresh")
            onClicked: if (root.controller) root.controller.refreshAll()
        }
    }

    StackLayout {
        id: viewStack

        Layout.fillWidth: true
        Layout.fillHeight: true
        currentIndex: 0

        // -- Active orders --------------------------------------------------
        ColumnLayout {
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: Style.pad + 10
                Layout.rightMargin: Style.pad + 10
                spacing: Style.gap

                Text { text: qsTr("Symbol"); color: Style.tertiaryText; font.pixelSize: Style.fontSizeSmall; Layout.preferredWidth: 130 }
                Text { text: qsTr("Side"); color: Style.tertiaryText; font.pixelSize: Style.fontSizeSmall; Layout.preferredWidth: 60 }
                Text { text: qsTr("Type"); color: Style.tertiaryText; font.pixelSize: Style.fontSizeSmall; Layout.preferredWidth: 80 }
                Text { text: qsTr("Price"); color: Style.tertiaryText; font.pixelSize: Style.fontSizeSmall; Layout.preferredWidth: 100 }
                Text { text: qsTr("Status"); color: Style.tertiaryText; font.pixelSize: Style.fontSizeSmall; Layout.fillWidth: true }
            }

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
                    height: 40
                    radius: Style.radius
                    color: hover.hovered ? Style.controlBackgroundHover
                                         : Style.contentBackground
                    border.width: 1
                    border.color: Style.separator

                    HoverHandler { id: hover }

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
                            elide: Text.ElideRight
                        }
                        Text {
                            text: model.side === undefined ? "" : model.side
                            color: model.side === "Buy" ? Style.success : Style.danger
                            font.pixelSize: 12
                            Layout.preferredWidth: 60
                        }
                        Text {
                            text: model.order_type === undefined ? "" : model.order_type
                            color: Style.secondaryText
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
                            color: root.statusColor(model.status === undefined ? "" : model.status)
                            font.pixelSize: 11
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }
                    }
                }

                EmptyState {
                    visible: activeList.count === 0
                    text: qsTr("No active orders")
                }
            }
        }

        // -- Recent fills ---------------------------------------------------
        ColumnLayout {
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: Style.pad + 10
                Layout.rightMargin: Style.pad + 10
                spacing: Style.gap

                Text { text: qsTr("Symbol"); color: Style.tertiaryText; font.pixelSize: Style.fontSizeSmall; Layout.preferredWidth: 130 }
                Text { text: qsTr("Status"); color: Style.tertiaryText; font.pixelSize: Style.fontSizeSmall; Layout.preferredWidth: 90 }
                Text { text: qsTr("Fill price"); color: Style.tertiaryText; font.pixelSize: Style.fontSizeSmall; Layout.preferredWidth: 110 }
                Text { text: qsTr("Filled at"); color: Style.tertiaryText; font.pixelSize: Style.fontSizeSmall; Layout.fillWidth: true }
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
                    height: 40
                    radius: Style.radius
                    color: hover2.hovered ? Style.controlBackgroundHover
                                          : Style.contentBackground
                    border.width: 1
                    border.color: Style.separator

                    HoverHandler { id: hover2 }

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
                            elide: Text.ElideRight
                        }
                        Text {
                            text: model.status === undefined ? "" : model.status
                            color: root.statusColor(model.status === undefined ? "" : model.status)
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
                            color: Style.secondaryText
                            font.pixelSize: 11
                            Layout.fillWidth: true
                            elide: Text.ElideRight
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
}
