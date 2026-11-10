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

    /// Non-zero spot currencies (engine-sorted by USDT value).
    readonly property var spotHoldings: account ? account.spotBalances : []

    /// Balances span many orders of magnitude (BTC vs a dust altcoin), so the
    /// number of decimals follows the magnitude instead of a fixed 2.
    function formatAmount(value) {
        if (!isFinite(value) || 0 === value) {
            return "0"
        }
        const magnitude = Math.abs(value)
        if (magnitude >= 1000) {
            return value.toFixed(2)
        }
        if (magnitude >= 1) {
            return value.toFixed(4)
        }
        if (magnitude >= 0.0001) {
            return value.toFixed(6)
        }
        return value.toExponential(4)
    }

    function formatValue(value) {
        if (!isFinite(value)) {
            return "—"
        }
        if (0 !== value && Math.abs(value) < 0.01) {
            return value.toPrecision(2)
        }
        return value.toFixed(2)
    }

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

        // Account header with the live holdings switch. The breakdown costs
        // extra REST pricing calls, so the control sits next to the data it
        // changes instead of three pages away in Settings.
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: Style.gap
            spacing: Style.gap

            Text {
                text: qsTr("Account")
                color: Style.secondaryText
                font.pixelSize: 12
            }

            Item { Layout.fillWidth: true }

            MacSwitch {
                id: holdingsSwitch
                text: qsTr("Alt-coin holdings")
                checked: root.controller ? root.controller.spotHoldingsEnabled : false
                onToggled: {
                    if (root.controller) {
                        root.controller.setSpotHoldingsEnabled(checked)
                    }
                }
            }
        }

        Connections {
            target: root.controller

            function onSpotHoldingsEnabledChanged() {
                holdingsSwitch.checked = root.controller.spotHoldingsEnabled
            }
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
                       ? "%1 %2".arg(root.account.futuresAvailable.toFixed(2))
                                 .arg(root.account.futuresCurrency)
                       : "—"
            }

            MetricTile {
                visible: root.cfdPresent
                label: qsTr("CFD equity")
                value: root.account
                       ? "%1 %2".arg(root.account.cfdTotal.toFixed(2))
                                 .arg(root.account.cfdCurrency)
                       : "—"
            }

            MetricTile {
                visible: root.cfdPresent
                label: qsTr("CFD available")
                value: root.account
                       ? "%1 %2".arg(root.account.cfdAvailable.toFixed(2))
                                 .arg(root.account.cfdCurrency)
                       : "—"
            }

            MetricTile {
                visible: root.cfdPresent
                label: qsTr("CFD margin used")
                value: root.account
                       ? "%1 %2".arg(root.account.cfdMargin.toFixed(2))
                                 .arg(root.account.cfdCurrency)
                       : "—"
            }

            MetricTile {
                visible: root.futuresPresent || root.cfdPresent
                label: qsTr("Unrealised PnL")
                value: root.account ? root.unrealised.toFixed(2) : "—"
                valueColor: root.unrealised >= 0 ? Style.success : Style.danger
            }
        }

        // -- Spot holdings --------------------------------------------------
        // One row per non-zero currency, valued from the engine's ticker
        // cache. A row without a cached price shows the amount and a dash.
        Text {
            visible: root.spotHoldings.length > 0
            text: qsTr("Spot holdings")
            color: Style.secondaryText
            font.pixelSize: 12
            Layout.topMargin: Style.gap
        }

        Rectangle {
            visible: root.spotHoldings.length > 0
            Layout.fillWidth: true
            implicitHeight: holdings.implicitHeight + 2 * Style.pad
            radius: Style.radius
            color: Style.contentBackground
            border.width: 1
            border.color: Style.separator

            ColumnLayout {
                id: holdings
                anchors.fill: parent
                anchors.margins: Style.pad
                spacing: Style.gapSmall

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Style.gap

                    Text {
                        Layout.preferredWidth: 96
                        text: qsTr("Currency")
                        color: Style.secondaryText
                        font.pixelSize: Style.fontSizeSmall
                    }

                    Text {
                        Layout.preferredWidth: 180
                        text: qsTr("Amount")
                        color: Style.secondaryText
                        font.pixelSize: Style.fontSizeSmall
                    }

                    Item { Layout.fillWidth: true }

                    Text {
                        text: qsTr("Value")
                        color: Style.secondaryText
                        font.pixelSize: Style.fontSizeSmall
                        horizontalAlignment: Text.AlignRight
                    }
                }

                Repeater {
                    model: root.spotHoldings

                    delegate: RowLayout {
                        Layout.fillWidth: true
                        spacing: Style.gap

                        Text {
                            Layout.preferredWidth: 96
                            text: modelData.currency
                            color: Style.text
                            font.pixelSize: Style.fontSize
                            font.weight: Font.DemiBold
                        }

                        Text {
                            Layout.preferredWidth: 180
                            text: root.formatAmount(modelData.total)
                            color: Style.secondaryText
                            font.pixelSize: Style.fontSize
                        }

                        Item { Layout.fillWidth: true }

                        Text {
                            text: modelData.hasValue
                                  ? root.formatValue(modelData.valueUsdt) + " USDT"
                                  : "—"
                            color: modelData.hasValue ? Style.text : Style.tertiaryText
                            font.pixelSize: Style.fontSize
                            horizontalAlignment: Text.AlignRight
                        }
                    }
                }
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
