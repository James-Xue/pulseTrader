import QtQuick
import QtQuick.Layouts
import Pulse.Client

/// Engine overview: identity, strategy counts and the account snapshot.
Flickable {
    id: root

    property var controller

    /// Account blocks are reported independently: a spot-only API key yields a
    /// spot block and no futures block. Tiles are rendered per present block so
    /// the dashboard never shows a futures "unknown" placeholder in front of a
    /// user whose balance is actually on the spot account.
    readonly property var account: root.controller ? root.controller.accountModel : null
    readonly property bool spotPresent: account ? account.spotPresent : false
    readonly property bool futuresPresent: account ? account.futuresPresent : false
    readonly property bool cfdPresent: account ? account.cfdPresent : false
    readonly property bool anyAccount: spotPresent || futuresPresent || cfdPresent
    readonly property double unrealised: futuresPresent
        ? account.futuresUnrealised
        : (cfdPresent ? account.cfdUnrealised : 0.0)

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
            color: Style.secondaryText
            font.pixelSize: 12
            Layout.topMargin: Style.gap
        }

        GridLayout {
            Layout.fillWidth: true
            columns: 3
            columnSpacing: Style.gap
            rowSpacing: Style.gap

            MetricTile {
                visible: root.spotPresent
                label: qsTr("Spot equity")
                value: root.account
                       ? "%1 %2".arg(root.account.spotTotal.toFixed(2))
                                 .arg(root.account.spotCurrency)
                       : "—"
            }

            MetricTile {
                visible: root.spotPresent
                label: qsTr("Spot available")
                value: root.account
                       ? "%1 %2".arg(root.account.spotAvailable.toFixed(2))
                                 .arg(root.account.spotCurrency)
                       : "—"
            }

            MetricTile {
                visible: root.futuresPresent
                label: qsTr("Futures equity")
                value: root.account
                       ? "%1 %2".arg(root.account.futuresTotal.toFixed(2))
                                 .arg(root.account.futuresCurrency)
                       : "—"
            }

            MetricTile {
                visible: root.futuresPresent
                label: qsTr("Futures available")
                value: root.account
                       ? root.account.futuresAvailable.toFixed(2) : "—"
            }

            MetricTile {
                visible: root.cfdPresent
                label: qsTr("CFD available")
                value: root.account
                       ? root.account.cfdAvailable.toFixed(2) : "—"
            }

            MetricTile {
                visible: root.futuresPresent || root.cfdPresent
                label: qsTr("Unrealised PnL")
                value: root.account ? root.unrealised.toFixed(2) : "—"
                valueColor: root.unrealised >= 0 ? Style.success : Style.danger
            }
        }

        Text {
            visible: root.controller && !root.anyAccount
            text: qsTr("The engine did not report any account balance yet.")
            color: Style.secondaryText
            font.pixelSize: Style.fontSizeSmall
        }

        Item { Layout.fillHeight: true }
    }
}
