import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Pulse.Client

/// Active orders and recent execution reports, filtered by market.
///
/// Two views of the same subject, so they switch with a macOS segmented
/// control rather than a tab bar. The market filter above applies to both:
/// the engine stamps `market_type` on every active order and every fill, so
/// a spot order is never read as a futures one. The two payloads carry
/// different timestamp units (milliseconds for active orders, nanoseconds for
/// reports), so each view renders its own `*_str` field instead of formatting
/// the raw number.
ColumnLayout {
    id: root

    property var controller

    /// 0 = all, 1 = spot, 2 = cfd, 3 = futures.
    property int marketIndex: 0

    readonly property var activeModels: root.controller
        ? [root.controller.ordersModel,
           root.controller.spotOrders,
           root.controller.cfdOrders,
           root.controller.futuresOrders]
        : []
    readonly property var reportModels: root.controller
        ? [root.controller.reportsModel,
           root.controller.spotReports,
           root.controller.cfdReports,
           root.controller.futuresReports]
        : []

    readonly property var activeModel: root.activeModels.length > root.marketIndex
                                       ? root.activeModels[root.marketIndex] : null
    readonly property var reportModel: root.reportModels.length > root.marketIndex
                                       ? root.reportModels[root.marketIndex] : null

    readonly property int activeCount: root.activeModel ? root.activeModel.count : 0
    readonly property int reportCount: root.reportModel ? root.reportModel.count : 0

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
    //
    // Two rows on purpose: the window's minimum content width (after the
    // sidebar) cannot hold the title, the market filter, the view switch and
    // the refresh button on one line.
    ColumnLayout {
        Layout.fillWidth: true
        Layout.margins: Style.pad
        spacing: Style.gapSmall

        RowLayout {
            Layout.fillWidth: true
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

            MacButton {
                text: qsTr("Refresh")
                onClicked: if (root.controller) root.controller.refreshAll()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Style.gap

            MacSegmentedControl {
                Layout.preferredWidth: 320
                items: [
                    qsTr("All (%1)").arg(root.controller ? root.controller.ordersModel.count : 0),
                    qsTr("Spot (%1)").arg(root.controller ? root.controller.spotOrders.count : 0),
                    qsTr("CFD (%1)").arg(root.controller ? root.controller.cfdOrders.count : 0),
                    qsTr("Futures (%1)").arg(root.controller ? root.controller.futuresOrders.count : 0)
                ]
                currentIndex: root.marketIndex
                onActivated: function (index) { root.marketIndex = index }
            }

            Item { Layout.fillWidth: true }

            MacSegmentedControl {
                Layout.preferredWidth: 240
                items: [qsTr("Active (%1)").arg(root.activeCount),
                        qsTr("Recent (%1)").arg(root.reportCount)]
                currentIndex: viewStack.currentIndex
                onActivated: function (index) { viewStack.currentIndex = index }
            }
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
                Text { text: qsTr("Filled / qty"); color: Style.tertiaryText; font.pixelSize: Style.fontSizeSmall; Layout.preferredWidth: 120 }
                Text { text: qsTr("Status"); color: Style.tertiaryText; font.pixelSize: Style.fontSizeSmall; Layout.preferredWidth: 90 }
                Text { text: qsTr("Market"); color: Style.tertiaryText; font.pixelSize: Style.fontSizeSmall; Layout.fillWidth: true }
            }

            ListView {
                id: activeList

                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.margins: Style.pad
                spacing: 6
                clip: true
                model: root.activeModel

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
                            // The engine serializes lowercase "buy" / "sell".
                            color: "buy" === model.side ? Style.success : Style.danger
                            font.pixelSize: 12
                            Layout.preferredWidth: 60
                        }
                        Text {
                            text: model.type === undefined ? "" : model.type
                            color: Style.secondaryText
                            font.pixelSize: 12
                            Layout.preferredWidth: 80
                        }
                        Text {
                            text: model.filled_qty === undefined ? ""
                                  : "%1 / %2".arg(Number(model.filled_qty))
                                                   .arg(Number(model.requested_qty))
                            color: Style.text
                            font.pixelSize: 12
                            Layout.preferredWidth: 120
                        }
                        Text {
                            text: model.status === undefined ? "" : model.status
                            color: root.statusColor(model.status === undefined ? "" : model.status)
                            font.pixelSize: 11
                            Layout.preferredWidth: 90
                        }
                        Text {
                            text: model.market_type === undefined ? "" : model.market_type
                            color: Style.tertiaryText
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
                Text { text: qsTr("Market"); color: Style.tertiaryText; font.pixelSize: Style.fontSizeSmall; Layout.preferredWidth: 90 }
                Text { text: qsTr("Filled at"); color: Style.tertiaryText; font.pixelSize: Style.fontSizeSmall; Layout.fillWidth: true }
            }

            ListView {
                id: reportList

                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.margins: Style.pad
                spacing: 6
                clip: true
                model: root.reportModel

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
                            // ExecutionReport serializes `final_status`, not
                            // `status`.
                            text: model.final_status === undefined ? "" : model.final_status
                            color: root.statusColor(model.final_status === undefined
                                                    ? "" : model.final_status)
                            font.pixelSize: 12
                            Layout.preferredWidth: 90
                        }
                        Text {
                            text: model.avg_fill_price === undefined ? ""
                                  : String(model.avg_fill_price)
                            color: Style.text
                            font.pixelSize: 12
                            Layout.preferredWidth: 110
                        }
                        Text {
                            text: model.market_type === undefined ? "" : model.market_type
                            color: Style.tertiaryText
                            font.pixelSize: 11
                            Layout.preferredWidth: 90
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