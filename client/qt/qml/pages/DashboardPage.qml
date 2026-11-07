import QtQuick
import QtQuick.Layouts
import Pulse.Client

/// Engine overview: identity, strategy counts and the account snapshot.
Flickable {
    id: root

    property var controller

    contentWidth: width
    contentHeight: content.implicitHeight + 2 * Style.pad
    clip: true

    ColumnLayout {
        id: content
        x: Style.pad
        y: Style.pad
        width: root.width - 2 * Style.pad
        spacing: Style.gap

        GridLayout {
            Layout.fillWidth: true
            columns: 4
            columnSpacing: Style.gap
            rowSpacing: Style.gap

            MetricTile {
                label: qsTr("Open positions")
                value: root.controller ? String(root.controller.openPositions) : "—"
            }

            MetricTile {
                label: qsTr("Strategies")
                value: root.controller
                       ? "%1 / %2".arg(root.controller.strategiesRunning)
                                    .arg(root.controller.strategiesTotal)
                       : "—"
            }

            MetricTile {
                label: qsTr("Network")
                value: root.controller ? root.controller.network : "—"
                valueColor: Style.accent
            }

            MetricTile {
                label: qsTr("Uptime")
                value: root.controller
                       ? qsTr("%1 min").arg(Math.round(root.controller.engineUptimeSec / 60))
                       : "—"
            }
        }

        Text {
            text: qsTr("Account")
            color: Style.muted
            font.pixelSize: 12
            Layout.topMargin: Style.gap
        }

        GridLayout {
            Layout.fillWidth: true
            columns: 3
            columnSpacing: Style.gap
            rowSpacing: Style.gap

            MetricTile {
                label: qsTr("Futures equity")
                present: root.controller ? root.controller.accountModel.futuresPresent : false
                value: root.controller
                       ? "%1 %2".arg(root.controller.accountModel.futuresTotal.toFixed(2))
                                 .arg(root.controller.accountModel.futuresCurrency)
                       : "—"
            }

            MetricTile {
                label: qsTr("Futures available")
                present: root.controller ? root.controller.accountModel.futuresPresent : false
                value: root.controller
                       ? root.controller.accountModel.futuresAvailable.toFixed(2) : "—"
            }

            MetricTile {
                label: qsTr("Unrealised PnL")
                present: root.controller ? root.controller.accountModel.futuresPresent : false
                value: root.controller
                       ? root.controller.accountModel.futuresUnrealised.toFixed(2) : "—"
                valueColor: {
                    if (!root.controller || !root.controller.accountModel.futuresPresent) {
                        return Style.muted
                    }
                    return root.controller.accountModel.futuresUnrealised >= 0
                           ? Style.good : Style.bad
                }
            }
        }

        Item { Layout.fillHeight: true }
    }
}
