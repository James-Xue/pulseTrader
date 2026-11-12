import QtQuick
import QtQuick.Layouts
import QtCore
import Pulse.Client

/// Engine overview, split into the three markets the engine actually trades.
///
/// The Gate app separates spot / CFD / futures because a balance, a position
/// and an order only mean something inside one of them. This page does the
/// same: one tab per market, and everything below the tab bar — balance tiles,
/// open positions, open orders — is read from that market's filtered view of
/// the single polled snapshot. Switching tabs therefore costs no request.
Flickable {
    id: root

    property var controller

    /// Last selected tab, restored across launches.
    Settings {
        id: dashboardSettings
        category: "dashboard"
        property string market: "spot"
    }

    readonly property var markets: ["spot", "cfd", "futures"]
    /// Clamped index into `markets`; an unknown persisted value falls back to
    /// spot instead of leaving every selection flag false.
    readonly property int marketIndex: Math.max(0, root.markets.indexOf(dashboardSettings.market))
    readonly property string selectedMarket: root.markets[root.marketIndex]
    readonly property bool spotSelected: 0 === root.marketIndex
    readonly property bool cfdSelected: 1 === root.marketIndex
    readonly property bool futuresSelected: 2 === root.marketIndex

    readonly property var account: root.controller ? root.controller.accountModel : null
    readonly property bool marketPresent: !root.account
        ? false
        : (root.spotSelected ? root.account.spotPresent
                             : (root.cfdSelected ? root.account.cfdPresent
                                                 : root.account.futuresPresent))

    /// Filtered views over the shared source models. The proxies carry the
    /// `count` property the tab labels bind to. They live on the controller,
    /// not on this page.
    readonly property var positionsModel: root.modelForMarket(
                                              root.controller ? root.controller.spotPositions : null,
                                              root.controller ? root.controller.cfdPositions : null,
                                              root.controller ? root.controller.futuresPositions : null)
    readonly property var ordersModel: root.modelForMarket(
                                           root.controller ? root.controller.spotOrders : null,
                                           root.controller ? root.controller.cfdOrders : null,
                                           root.controller ? root.controller.futuresOrders : null)

    readonly property int spotPositionCount: root.controller ? root.controller.spotPositions.count : 0
    readonly property int cfdPositionCount: root.controller ? root.controller.cfdPositions.count : 0
    readonly property int futuresPositionCount: root.controller ? root.controller.futuresPositions.count : 0

    readonly property int positionCount: root.positionsModel ? root.positionsModel.count : 0
    readonly property int orderCount: root.ordersModel ? root.ordersModel.count : 0

    /// Spot holdings are only meaningful on the spot tab.
    readonly property var spotHoldings: root.controller && root.spotSelected
                                        ? root.controller.accountModel.spotBalances : []

    /// Unrealised PnL of the selected market only.
    readonly property double unrealised: !root.account
        ? 0.0
        : (root.futuresSelected ? root.account.futuresUnrealised
                                : (root.cfdSelected ? root.account.cfdUnrealised : 0.0))

    function modelForMarket(spotModel, cfdModel, futuresModel) {
        if (root.spotSelected) {
            return spotModel
        }
        if (root.cfdSelected) {
            return cfdModel
        }
        return futuresModel
    }

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

    function marketLabel(market) {
        if ("spot" === market) {
            return qsTr("Spot")
        }
        if ("cfd" === market) {
            return qsTr("CFD")
        }
        return qsTr("Futures")
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

        // -- Engine-wide tiles (independent of the selected market) ----------
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

        // -- Market tabs -----------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: Style.gapSmall
            spacing: Style.gap

            Text {
                text: qsTr("Market")
                color: Style.secondaryText
                font.pixelSize: 12
            }

            MacSegmentedControl {
                id: marketTabs
                Layout.preferredWidth: 380
                items: [
                    qsTr("Spot (%1)").arg(root.spotPositionCount),
                    qsTr("CFD (%1)").arg(root.cfdPositionCount),
                    qsTr("Futures (%1)").arg(root.futuresPositionCount)
                ]
                currentIndex: root.marketIndex
                onActivated: function (index) {
                    dashboardSettings.market = root.markets[index]
                }
            }

            Item { Layout.fillWidth: true }

            // The spot breakdown costs extra REST pricing calls, so the
            // control lives next to the data it changes — and only on the
            // spot tab, where the table is rendered.
            MacSwitch {
                id: holdingsSwitch
                visible: root.spotSelected
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

        // -- Balance tiles for the selected market ---------------------------
        GridLayout {
            Layout.fillWidth: true
            columns: 4
            columnSpacing: Style.gap
            rowSpacing: Style.gap

            MetricTile {
                visible: root.spotSelected
                label: qsTr("Spot equity")
                present: root.account ? root.account.spotPresent : false
                value: root.account
                       ? "%1 %2".arg(root.account.spotTotal.toFixed(2))
                                 .arg(root.account.spotCurrency)
                       : "—"
            }

            MetricTile {
                visible: root.spotSelected
                label: qsTr("Spot available")
                present: root.account ? root.account.spotPresent : false
                value: root.account
                       ? "%1 %2".arg(root.account.spotAvailable.toFixed(2))
                                 .arg(root.account.spotCurrency)
                       : "—"
            }

            MetricTile {
                visible: root.cfdSelected
                label: qsTr("CFD equity")
                present: root.account ? root.account.cfdPresent : false
                value: root.account
                       ? "%1 %2".arg(root.account.cfdTotal.toFixed(2))
                                 .arg(root.account.cfdCurrency)
                       : "—"
            }

            MetricTile {
                visible: root.cfdSelected
                label: qsTr("CFD available")
                present: root.account ? root.account.cfdPresent : false
                value: root.account
                       ? "%1 %2".arg(root.account.cfdAvailable.toFixed(2))
                                 .arg(root.account.cfdCurrency)
                       : "—"
            }

            MetricTile {
                visible: root.cfdSelected
                label: qsTr("CFD margin used")
                present: root.account ? root.account.cfdPresent : false
                value: root.account
                       ? "%1 %2".arg(root.account.cfdMargin.toFixed(2))
                                 .arg(root.account.cfdCurrency)
                       : "—"
            }

            MetricTile {
                visible: root.futuresSelected
                label: qsTr("Futures equity")
                present: root.account ? root.account.futuresPresent : false
                value: root.account
                       ? "%1 %2".arg(root.account.futuresTotal.toFixed(2))
                                 .arg(root.account.futuresCurrency)
                       : "—"
            }

            MetricTile {
                visible: root.futuresSelected
                label: qsTr("Futures available")
                present: root.account ? root.account.futuresPresent : false
                value: root.account
                       ? "%1 %2".arg(root.account.futuresAvailable.toFixed(2))
                                 .arg(root.account.futuresCurrency)
                       : "—"
            }

            MetricTile {
                visible: root.futuresSelected
                label: qsTr("Position margin")
                present: root.account ? root.account.futuresPresent : false
                value: root.account
                       ? "%1 %2".arg(root.account.futuresPositionMargin.toFixed(2))
                                 .arg(root.account.futuresCurrency)
                       : "—"
            }

            MetricTile {
                visible: root.cfdSelected || root.futuresSelected
                label: qsTr("Unrealised PnL")
                present: root.marketPresent
                value: root.account ? root.unrealised.toFixed(2) : "—"
                valueColor: root.unrealised >= 0 ? Style.success : Style.danger
            }
        }

        Text {
            Layout.fillWidth: true
            visible: root.controller && !root.marketPresent
            text: qsTr("The engine did not report a %1 balance yet.")
                  .arg(root.marketLabel(root.selectedMarket))
            color: Style.secondaryText
            font.pixelSize: Style.fontSizeSmall
        }

        // -- Open positions, one market only ---------------------------------
        Text {
            Layout.fillWidth: true
            Layout.topMargin: Style.gapSmall
            text: qsTr("Positions · %1 (%2)")
                  .arg(root.marketLabel(root.selectedMarket))
                  .arg(root.positionCount)
            color: Style.secondaryText
            font.pixelSize: 12
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: positionsColumn.implicitHeight + 2 * Style.pad
            radius: Style.radius
            color: Style.contentBackground
            border.width: 1
            border.color: Style.separator

            ColumnLayout {
                id: positionsColumn
                anchors.fill: parent
                anchors.margins: Style.pad
                spacing: Style.gapSmall

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Style.gap

                    Text { Layout.preferredWidth: 130; text: qsTr("Symbol"); color: Style.secondaryText; font.pixelSize: Style.fontSizeSmall }
                    Text { Layout.preferredWidth: 60; text: qsTr("Side"); color: Style.secondaryText; font.pixelSize: Style.fontSizeSmall }
                    Text { Layout.preferredWidth: 100; text: qsTr("Qty"); color: Style.secondaryText; font.pixelSize: Style.fontSizeSmall }
                    Text { Layout.preferredWidth: 110; text: qsTr("Entry"); color: Style.secondaryText; font.pixelSize: Style.fontSizeSmall }
                    Text { Layout.preferredWidth: 110; text: qsTr("uPnL"); color: Style.secondaryText; font.pixelSize: Style.fontSizeSmall }
                    Item { Layout.fillWidth: true }
                }

                Repeater {
                    model: root.positionsModel ? root.positionsModel : []

                    delegate: RowLayout {
                        Layout.fillWidth: true
                        spacing: Style.gap

                        Text {
                            Layout.preferredWidth: 130
                            text: model.symbol === undefined ? "" : model.symbol
                            color: Style.text
                            font.pixelSize: 12
                            elide: Text.ElideRight
                        }
                        Text {
                            Layout.preferredWidth: 60
                            text: model.side === undefined ? "" : model.side
                            color: "buy" === model.side ? Style.success : Style.danger
                            font.pixelSize: 12
                        }
                        Text {
                            Layout.preferredWidth: 100
                            text: model.quantity === undefined ? "" : String(model.quantity)
                            color: Style.text
                            font.pixelSize: 12
                        }
                        Text {
                            Layout.preferredWidth: 110
                            text: model.entry_price === undefined ? "" : String(model.entry_price)
                            color: Style.text
                            font.pixelSize: 12
                        }
                        Text {
                            Layout.preferredWidth: 110
                            text: model.unrealized_pnl === undefined
                                  ? "" : Number(model.unrealized_pnl).toFixed(2)
                            color: Number(model.unrealized_pnl) >= 0 ? Style.success : Style.danger
                            font.pixelSize: 12
                        }
                        Item { Layout.fillWidth: true }
                    }
                }

                Text {
                    Layout.fillWidth: true
                    visible: 0 === root.positionCount
                    text: qsTr("No open positions on this market")
                    color: Style.tertiaryText
                    font.pixelSize: Style.fontSizeSmall
                }
            }
        }

        // -- Open orders, one market only ------------------------------------
        Text {
            Layout.fillWidth: true
            Layout.topMargin: Style.gapSmall
            text: qsTr("Open orders · %1 (%2)")
                  .arg(root.marketLabel(root.selectedMarket))
                  .arg(root.orderCount)
            color: Style.secondaryText
            font.pixelSize: 12
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: ordersColumn.implicitHeight + 2 * Style.pad
            radius: Style.radius
            color: Style.contentBackground
            border.width: 1
            border.color: Style.separator

            ColumnLayout {
                id: ordersColumn
                anchors.fill: parent
                anchors.margins: Style.pad
                spacing: Style.gapSmall

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Style.gap

                    Text { Layout.preferredWidth: 130; text: qsTr("Symbol"); color: Style.secondaryText; font.pixelSize: Style.fontSizeSmall }
                    Text { Layout.preferredWidth: 60; text: qsTr("Side"); color: Style.secondaryText; font.pixelSize: Style.fontSizeSmall }
                    Text { Layout.preferredWidth: 80; text: qsTr("Type"); color: Style.secondaryText; font.pixelSize: Style.fontSizeSmall }
                    Text { Layout.preferredWidth: 110; text: qsTr("Filled / qty"); color: Style.secondaryText; font.pixelSize: Style.fontSizeSmall }
                    Text { Layout.fillWidth: true; text: qsTr("Status"); color: Style.secondaryText; font.pixelSize: Style.fontSizeSmall }
                }

                Repeater {
                    model: root.ordersModel ? root.ordersModel : []

                    delegate: RowLayout {
                        Layout.fillWidth: true
                        spacing: Style.gap

                        Text {
                            Layout.preferredWidth: 130
                            text: model.symbol === undefined ? "" : model.symbol
                            color: Style.text
                            font.pixelSize: 12
                            elide: Text.ElideRight
                        }
                        Text {
                            Layout.preferredWidth: 60
                            text: model.side === undefined ? "" : model.side
                            color: "buy" === model.side ? Style.success : Style.danger
                            font.pixelSize: 12
                        }
                        Text {
                            Layout.preferredWidth: 80
                            text: model.type === undefined ? "" : model.type
                            color: Style.secondaryText
                            font.pixelSize: 12
                        }
                        Text {
                            Layout.preferredWidth: 110
                            text: model.filled_qty === undefined
                                  ? "" : "%1 / %2".arg(Number(model.filled_qty))
                                                   .arg(Number(model.requested_qty))
                            color: Style.text
                            font.pixelSize: 12
                        }
                        Text {
                            Layout.fillWidth: true
                            text: model.status === undefined ? "" : model.status
                            color: Style.secondaryText
                            font.pixelSize: 12
                            elide: Text.ElideRight
                        }
                    }
                }

                Text {
                    Layout.fillWidth: true
                    visible: 0 === root.orderCount
                    text: qsTr("No open orders on this market")
                    color: Style.tertiaryText
                    font.pixelSize: Style.fontSizeSmall
                }
            }
        }

        // -- Spot holdings (spot tab only) -----------------------------------
        Text {
            visible: root.spotSelected && root.spotHoldings.length > 0
            text: qsTr("Spot holdings")
            color: Style.secondaryText
            font.pixelSize: 12
            Layout.topMargin: Style.gapSmall
        }

        Rectangle {
            visible: root.spotSelected && root.spotHoldings.length > 0
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

        Item { Layout.fillHeight: true }
    }
}