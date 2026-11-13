import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Pulse.Client

/// Gold Traders — read-only view of the engine's XAUUSD strategy instances.
///
/// The page answers "which gold traders exist, which one is selected, what is
/// it configured with, and is anything attached to the data feed". v1 has no
/// write surface: it never calls set_strategy_param, set_strategy_trading or
/// any order method. Editing is deferred to the Gold Trader Center.
///
/// The layout is a single-column Flickable rather than master/detail: at the
/// 860 px window floor the page is ~651 px wide, which cannot host two
/// readable columns, and a nested ListView inside this Flickable would fight
/// the outer scroll. The instance count is single-digit, so a Repeater is
/// enough.
Flickable {
    id: root

    property var controller
    property var dataServer

    readonly property var selected: root.controller ? root.controller.selectedStrategy : ({})
    readonly property bool hasSelection: !!root.selected && root.selected.id !== undefined
    readonly property var risk: root.controller ? root.controller.riskModel : null
    readonly property var paramsModel: root.controller ? root.controller.strategyParamsModel : null
    readonly property int goldCount: root.controller ? root.controller.goldStrategies.count : 0
    readonly property int totalCount: root.controller ? root.controller.strategiesModel.count : 0
    readonly property int hiddenCount: Math.max(0, root.totalCount - root.goldCount)
    readonly property var consumers: root.dataServer ? root.dataServer.clients : []

    contentWidth: width
    contentHeight: content.implicitHeight + 2 * Style.pad
    clip: true

    /// pulse::ErrorCode values that can appear as a haltReason. The constants
    /// are duplicated here on purpose: the client never links engine code.
    function haltReasonLabel(code) {
        switch (code) {
        case 0:
            return qsTr("None")
        case 3001:
            return qsTr("Drawdown limit")
        case 3004:
            return qsTr("Stop loss")
        case 3005:
            return qsTr("Take profit")
        case 3007:
            return qsTr("Manual halt")
        case 3008:
            return qsTr("Inactive market")
        default:
            return qsTr("Code %1").arg(code)
        }
    }

    /// Engine parameter names are protocol identifiers, not UI text: display
    /// labels live here and an unknown key falls back to the raw name.
    function paramLabel(key) {
        switch (key) {
        case "order_quantity":
            return qsTr("Order quantity")
        case "min_confidence":
            return qsTr("Min confidence")
        case "ema_fast_period":
            return qsTr("EMA fast period")
        case "ema_slow_period":
            return qsTr("EMA slow period")
        case "bb_period":
            return qsTr("Bollinger period")
        case "bb_std_dev":
            return qsTr("Bollinger std dev")
        case "ob_imbalance_threshold":
            return qsTr("Order-book imbalance")
        case "ob_depth":
            return qsTr("Order-book depth")
        case "supertrend_period":
            return qsTr("Supertrend period")
        case "supertrend_multiplier":
            return qsTr("Supertrend multiplier")
        case "cooldown_seconds":
            return qsTr("Cooldown (s)")
        case "stop_loss_pct":
            return qsTr("Stop loss (%)")
        case "take_profit_pct":
            return qsTr("Take profit (%)")
        case "auto_trade":
            return qsTr("Auto trade")
        default:
            return key
        }
    }

    /// Drawdowns arrive as fractions of equity (0.05 = 5%).
    function fmtPct(value) {
        if (value === undefined || value === null || !isFinite(value)) {
            return "—"
        }
        return (Number(value) * 100).toFixed(2) + "%"
    }

    function fmtMoney(value) {
        if (value === undefined || value === null || !isFinite(value)) {
            return "—"
        }
        return Number(value).toFixed(2)
    }

    /// USD amount with the "U" unit used by the gold trader thresholds.
    function fmtU(value) {
        if (value === undefined || value === null || !isFinite(value)) {
            return "—"
        }
        return Number(value).toFixed(2) + " U"
    }

    /// Plain number without forced decimals (periods, multipliers, lots).
    function fmtNum(value) {
        if (value === undefined || value === null || !isFinite(value)) {
            return "—"
        }
        return Number(value).toString()
    }

    /// Price with XAUUSD's two-decimal quote precision.
    function fmtPrice(value) {
        if (value === undefined || value === null || !isFinite(value)) {
            return "—"
        }
        return Number(value).toFixed(2)
    }

    function yesNo(value) {
        return value ? qsTr("Yes") : qsTr("No")
    }

    ColumnLayout {
        id: content

        x: Style.pad
        y: Style.pad
        width: root.width - 2 * Style.pad
        spacing: Style.gap

        // -- Toolbar ---------------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            spacing: Style.gap

            Text {
                text: qsTr("Gold Traders")
                color: Style.text
                font.pixelSize: Style.titleSize
                font.weight: Font.DemiBold
            }

            Text {
                text: qsTr("%1 of %2 strategies").arg(root.goldCount).arg(root.totalCount)
                color: Style.secondaryText
                font.pixelSize: Style.fontSizeSmall
            }

            Item { Layout.fillWidth: true }

            MacButton {
                text: qsTr("Refresh")
                onClicked: if (root.controller) root.controller.refreshAll()
            }
        }

        // -- L7 dynamic stop supervisor --------------------------------------
        //
        // The gold trader is an L7 exit supervisor, not a StrategyBase
        // instance, so it does not appear in the trader list below. This
        // card is its read-only status: armed/disabled, the configured USD
        // thresholds, and every XAUUSD CFD position it currently manages
        // (mark, peak PnL, effective stop, exchange-side stop).
        Rectangle {
            id: goldCard

            Layout.fillWidth: true
            implicitHeight: goldStatusColumn.implicitHeight + 2 * Style.pad
            radius: Style.radius
            color: Style.contentBackground
            border.width: 1
            border.color: Style.separator

            readonly property var gold: root.controller ? root.controller.goldTrader : ({})
            readonly property var goldConfig: gold && gold.config ? gold.config : ({})
            readonly property var goldPositions:
                gold && gold.tracked_positions ? gold.tracked_positions : []
            readonly property bool armed: gold && gold.armed === true

            ColumnLayout {
                id: goldStatusColumn

                anchors.fill: parent
                anchors.margins: Style.pad
                spacing: Style.gapSmall

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Style.gap

                    Text {
                        text: qsTr("L7 dynamic stop")
                        color: Style.text
                        font.pixelSize: Style.fontSize
                        font.weight: Font.DemiBold
                    }

                    Rectangle {
                        implicitWidth: armedLabel.implicitWidth + 14
                        implicitHeight: 20
                        radius: 4
                        color: goldCard.armed ? Style.success : Style.tertiaryText

                        Text {
                            id: armedLabel
                            anchors.centerIn: parent
                            text: goldCard.armed ? qsTr("Armed") : qsTr("Disabled")
                            color: "white"
                            font.pixelSize: Style.fontSizeSmall
                            font.weight: Font.DemiBold
                        }
                    }

                    Text {
                        text: qsTr("XAUUSD CFD · USD thresholds")
                        color: Style.tertiaryText
                        font.pixelSize: Style.fontSizeSmall
                    }

                    Item { Layout.fillWidth: true }

                    Text {
                        visible: !goldCard.armed
                        text: goldCard.gold && goldCard.gold.reason
                              ? goldCard.gold.reason : qsTr("Not armed")
                        color: Style.warning
                        font.pixelSize: Style.fontSizeSmall
                        elide: Text.ElideRight
                    }
                }

                GridLayout {
                    Layout.fillWidth: true
                    columns: 4
                    columnSpacing: Style.gap
                    rowSpacing: Style.gapSmall

                    Text { text: qsTr("Hard stop"); color: Style.secondaryText; font.pixelSize: 11 }
                    Text { text: qsTr("Break-even after"); color: Style.secondaryText; font.pixelSize: 11 }
                    Text { text: qsTr("Peak give-back"); color: Style.secondaryText; font.pixelSize: 11 }
                    Text { text: qsTr("Give-back arms"); color: Style.secondaryText; font.pixelSize: 11 }

                    Text {
                        text: root.fmtU(goldCard.goldConfig.hard_stop_usd)
                        color: Style.text; font.pixelSize: 13
                    }
                    Text {
                        text: root.fmtU(goldCard.goldConfig.breakeven_trigger_usd)
                        color: Style.text; font.pixelSize: 13
                    }
                    Text {
                        text: root.fmtU(goldCard.goldConfig.peak_drawdown_usd)
                        color: Style.text; font.pixelSize: 13
                    }
                    Text {
                        text: root.fmtU(goldCard.goldConfig.peak_trail_arm_usd)
                        color: Style.text; font.pixelSize: 13
                    }

                    Text { text: qsTr("SuperTrend"); color: Style.secondaryText; font.pixelSize: 11 }
                    Text { text: qsTr("ST buffer"); color: Style.secondaryText; font.pixelSize: 11 }
                    Text { text: qsTr("Contract volume"); color: Style.secondaryText; font.pixelSize: 11 }
                    Text { text: qsTr("Exchange stop"); color: Style.secondaryText; font.pixelSize: 11 }

                    Text {
                        text: qsTr("%1 × %2")
                                  .arg(root.fmtNum(goldCard.goldConfig.supertrend_period))
                                  .arg(root.fmtNum(goldCard.goldConfig.supertrend_multiplier))
                        color: Style.text; font.pixelSize: 13
                    }
                    Text {
                        text: root.fmtU(goldCard.goldConfig.supertrend_buffer_usd)
                        color: Style.text; font.pixelSize: 13
                    }
                    Text {
                        text: qsTr("%1 oz/lot").arg(root.fmtNum(goldCard.goldConfig.contract_volume))
                        color: Style.text; font.pixelSize: 13
                    }
                    Text {
                        text: goldCard.goldConfig.exchange_stop ? qsTr("On") : qsTr("Off")
                        color: goldCard.goldConfig.exchange_stop
                               ? Style.success : Style.secondaryText
                        font.pixelSize: 13
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 1
                    color: Style.separator
                }

                Text {
                    text: qsTr("Managed positions (%1)")
                              .arg(goldCard.goldPositions.length)
                    color: Style.secondaryText
                    font.pixelSize: Style.fontSizeSmall
                }

                Text {
                    Layout.fillWidth: true
                    visible: 0 === goldCard.goldPositions.length
                    text: qsTr("No open XAUUSD CFD position. The supervisor arms when one appears.")
                    color: Style.tertiaryText
                    font.pixelSize: Style.fontSizeSmall
                    wrapMode: Text.WordWrap
                }

                Repeater {
                    model: goldCard.goldPositions

                    delegate: ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Style.gap

                            Text {
                                text: modelData.position_id === undefined
                                      ? "" : modelData.position_id
                                color: Style.text
                                font.pixelSize: 12
                                font.weight: Font.DemiBold
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                            Text {
                                text: qsTr("%1 · %2 lots")
                                          .arg(modelData.side === undefined ? "" : modelData.side)
                                          .arg(root.fmtNum(modelData.quantity))
                                color: Style.secondaryText
                                font.pixelSize: 11
                            }
                            Text {
                                text: qsTr("entry %1 → %2")
                                          .arg(root.fmtPrice(modelData.entry_price))
                                          .arg(root.fmtPrice(modelData.mark_price))
                                color: Style.secondaryText
                                font.pixelSize: 11
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Style.gap

                            Text {
                                text: qsTr("PnL %1").arg(root.fmtU(modelData.unrealized_pnl))
                                color: Number(modelData.unrealized_pnl) >= 0
                                       ? Style.success : Style.danger
                                font.pixelSize: 11
                            }
                            Text {
                                text: qsTr("peak %1").arg(root.fmtU(modelData.peak_pnl))
                                color: Style.secondaryText
                                font.pixelSize: 11
                            }
                            Text {
                                text: qsTr("stop %1").arg(root.fmtPrice(modelData.effective_stop))
                                color: Style.text
                                font.pixelSize: 11
                            }
                            Text {
                                text: qsTr("exchange %1").arg(root.fmtPrice(modelData.exchange_stop))
                                color: Style.secondaryText
                                font.pixelSize: 11
                            }
                        }
                    }
                }
            }
        }

        // -- Trader list -----------------------------------------------------
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: tradersColumn.implicitHeight + 2 * Style.pad
            radius: Style.radius
            color: Style.contentBackground
            border.width: 1
            border.color: Style.separator

            ColumnLayout {
                id: tradersColumn

                anchors.fill: parent
                anchors.margins: Style.pad
                spacing: Style.gapSmall

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Style.gap

                    Text {
                        text: qsTr("Traders")
                        color: Style.secondaryText
                        font.pixelSize: Style.fontSizeSmall
                    }

                    Item { Layout.fillWidth: true }

                    Text {
                        visible: root.hiddenCount > 0
                        text: qsTr("%1 non-gold hidden").arg(root.hiddenCount)
                        color: Style.tertiaryText
                        font.pixelSize: Style.fontSizeSmall
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Style.gap

                    Text {
                        Layout.preferredWidth: 200
                        text: qsTr("Instance")
                        color: Style.secondaryText
                        font.pixelSize: 11
                    }
                    Text {
                        Layout.preferredWidth: 130
                        text: qsTr("Strategy")
                        color: Style.secondaryText
                        font.pixelSize: 11
                    }
                    Text {
                        Layout.preferredWidth: 80
                        text: qsTr("Symbol")
                        color: Style.secondaryText
                        font.pixelSize: 11
                    }
                    Text {
                        Layout.preferredWidth: 70
                        text: qsTr("Poll")
                        color: Style.secondaryText
                        font.pixelSize: 11
                    }
                    Item { Layout.fillWidth: true }
                }

                Repeater {
                    model: root.controller ? root.controller.goldStrategies : 0

                    delegate: Rectangle {
                        Layout.fillWidth: true
                        // ColumnLayout sizes children from implicitHeight /
                        // Layout.preferredHeight; a bare `height` is ignored.
                        Layout.preferredHeight: 44
                        radius: Style.radius
                        color: model.id === (root.controller
                                             ? root.controller.selectedStrategyId : "")
                               ? Style.controlBackgroundHover : "transparent"
                        border.width: 1
                        border.color: Style.separator

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 10
                            spacing: Style.gap

                            Text {
                                Layout.preferredWidth: 200
                                text: model.id === undefined ? "" : model.id
                                color: Style.text
                                font.pixelSize: 12
                                elide: Text.ElideRight
                            }
                            Text {
                                Layout.preferredWidth: 130
                                text: model.name === undefined ? "" : model.name
                                color: Style.secondaryText
                                font.pixelSize: 12
                                elide: Text.ElideRight
                            }
                            Text {
                                Layout.preferredWidth: 80
                                text: model.symbol === undefined ? "" : model.symbol
                                color: Style.secondaryText
                                font.pixelSize: 12
                            }
                            Text {
                                Layout.preferredWidth: 70
                                text: model.poll_interval_ms === undefined
                                      ? "" : qsTr("%1 ms").arg(model.poll_interval_ms)
                                color: Style.secondaryText
                                font.pixelSize: 12
                            }

                            Item { Layout.fillWidth: true }

                            Text {
                                visible: model.paused === true
                                text: qsTr("paused")
                                color: Style.warning
                                font.pixelSize: 11
                            }
                            Text {
                                visible: model.auto_trade === true
                                text: qsTr("auto")
                                color: Style.accent
                                font.pixelSize: 11
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: if (root.controller) root.controller.selectStrategy(model.id)
                        }
                    }
                }

                Text {
                    Layout.fillWidth: true
                    visible: 0 === root.goldCount
                    text: root.controller
                          ? qsTr("No XAUUSD strategies on this engine.")
                          : qsTr("Waiting for the engine.")
                    color: Style.tertiaryText
                    font.pixelSize: Style.fontSizeSmall
                }

                Text {
                    Layout.fillWidth: true
                    visible: 0 === root.goldCount && root.controller
                    text: qsTr("A client-launched engine configures BTC_USDT instances only.")
                    color: Style.tertiaryText
                    font.pixelSize: Style.fontSizeSmall
                    wrapMode: Text.WordWrap
                }
            }
        }

        // -- Identity & connection -------------------------------------------
        Rectangle {
            Layout.fillWidth: true
            visible: root.hasSelection
            implicitHeight: identityColumn.implicitHeight + 2 * Style.pad
            radius: Style.radius
            color: Style.contentBackground
            border.width: 1
            border.color: Style.separator

            ColumnLayout {
                id: identityColumn

                anchors.fill: parent
                anchors.margins: Style.pad
                spacing: Style.gapSmall

                Text {
                    text: qsTr("Identity & connection")
                    color: Style.secondaryText
                    font.pixelSize: Style.fontSizeSmall
                }

                GridLayout {
                    Layout.fillWidth: true
                    columns: 2
                    columnSpacing: Style.gap
                    rowSpacing: Style.gap

                    Text { text: qsTr("Instance"); color: Style.secondaryText; font.pixelSize: 12 }
                    Text { text: root.selected.id === undefined ? "—" : root.selected.id; color: Style.text; font.pixelSize: 13 }

                    Text { text: qsTr("Strategy class"); color: Style.secondaryText; font.pixelSize: 12 }
                    Text { text: root.selected.name === undefined ? "—" : root.selected.name; color: Style.text; font.pixelSize: 13 }

                    Text { text: qsTr("Symbol"); color: Style.secondaryText; font.pixelSize: 12 }
                    Text { text: root.selected.symbol === undefined ? "—" : root.selected.symbol; color: Style.text; font.pixelSize: 13 }

                    Text { text: qsTr("Poll interval"); color: Style.secondaryText; font.pixelSize: 12 }
                    Text {
                        text: root.selected.poll_interval_ms === undefined
                              ? "—" : qsTr("%1 ms").arg(root.selected.poll_interval_ms)
                        color: Style.text
                        font.pixelSize: 13
                    }

                    // list_strategies carries no market_type, so the market is
                    // reported engine-wide rather than inferred per instance.
                    Text { text: qsTr("Market (engine-wide)"); color: Style.secondaryText; font.pixelSize: 12 }
                    Text {
                        text: root.controller ? root.controller.activeMarket : "—"
                        color: Style.text
                        font.pixelSize: 13
                    }

                    Text { text: qsTr("Enabled"); color: Style.secondaryText; font.pixelSize: 12 }
                    Text { text: root.yesNo(root.selected.enabled === true); color: Style.text; font.pixelSize: 13 }

                    Text { text: qsTr("Running"); color: Style.secondaryText; font.pixelSize: 12 }
                    Text { text: root.yesNo(root.selected.running === true); color: Style.text; font.pixelSize: 13 }

                    Text { text: qsTr("Paused"); color: Style.secondaryText; font.pixelSize: 12 }
                    Text {
                        text: root.yesNo(root.selected.paused === true)
                        color: root.selected.paused === true ? Style.warning : Style.text
                        font.pixelSize: 13
                    }

                    Text { text: qsTr("Auto trade"); color: Style.secondaryText; font.pixelSize: 12 }
                    Text {
                        text: root.yesNo(root.selected.auto_trade === true)
                        color: root.selected.auto_trade === true ? Style.accent : Style.text
                        font.pixelSize: 13
                    }

                    Text { text: qsTr("Data server"); color: Style.secondaryText; font.pixelSize: 12 }
                    Text {
                        text: root.dataServer
                              ? qsTr("%1 / %2").arg(root.dataServer.connectedCount)
                                              .arg(root.dataServer.maxClients)
                              : "—"
                        color: Style.text
                        font.pixelSize: 13
                    }

                    Text { text: qsTr("Data feed"); color: Style.secondaryText; font.pixelSize: 12 }
                    Text {
                        text: root.dataServer && root.dataServer.engineReady
                              ? qsTr("ready") : qsTr("waiting")
                        color: root.dataServer && root.dataServer.engineReady
                               ? Style.success : Style.warning
                        font.pixelSize: 13
                    }
                }

                Text {
                    Layout.topMargin: Style.gapSmall
                    text: qsTr("Attached consumers")
                    color: Style.secondaryText
                    font.pixelSize: Style.fontSizeSmall
                }

                Column {
                    Layout.fillWidth: true
                    spacing: 2
                    visible: root.dataServer && root.dataServer.connectedCount > 0

                    Repeater {
                        model: root.consumers

                        delegate: Row {
                            required property var modelData
                            width: parent.width
                            spacing: Style.gap

                            Text {
                                width: 140
                                text: modelData.client_id
                                color: Style.text
                                font.pixelSize: Style.fontSizeSmall
                                elide: Text.ElideRight
                            }
                            Text {
                                width: 160
                                text: modelData.strategy && modelData.strategy.length > 0
                                      ? modelData.strategy : qsTr("(no strategy)")
                                color: Style.secondaryText
                                font.pixelSize: Style.fontSizeSmall
                                elide: Text.ElideRight
                            }
                            Text {
                                text: qsTr("last seen %1").arg(
                                          Qt.formatTime(new Date(modelData.last_seen_ms),
                                                        "hh:mm:ss"))
                                color: Style.tertiaryText
                                font.pixelSize: Style.fontSizeSmall
                            }
                            Text {
                                // The hello's strategy field is free text; the
                                // gold bot sends the instance id it consumes.
                                visible: modelData.strategy === root.controller.selectedStrategyId
                                text: qsTr("linked")
                                color: Style.accent
                                font.pixelSize: Style.fontSizeSmall
                            }
                        }
                    }
                }

                Text {
                    Layout.fillWidth: true
                    visible: !root.dataServer || root.dataServer.connectedCount === 0
                    text: qsTr("No consumers connected.")
                    color: Style.tertiaryText
                    font.pixelSize: Style.fontSizeSmall
                }
            }
        }

        // -- Parameters ------------------------------------------------------
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: paramsColumn.implicitHeight + 2 * Style.pad
            radius: Style.radius
            color: Style.contentBackground
            border.width: 1
            border.color: Style.separator

            ColumnLayout {
                id: paramsColumn

                anchors.fill: parent
                anchors.margins: Style.pad
                spacing: Style.gapSmall

                Text {
                    text: qsTr("Parameters")
                    color: Style.secondaryText
                    font.pixelSize: Style.fontSizeSmall
                }

                Text {
                    visible: !root.hasSelection
                    text: qsTr("Select a trader to see its parameters.")
                    color: Style.tertiaryText
                    font.pixelSize: Style.fontSizeSmall
                }

                RowLayout {
                    Layout.fillWidth: true
                    visible: root.hasSelection && root.paramsModel
                             && root.paramsModel.error.length > 0
                    spacing: Style.gap

                    Text {
                        Layout.fillWidth: true
                        text: root.paramsModel
                              ? qsTr("Parameters unavailable: %1").arg(root.paramsModel.error)
                              : ""
                        color: Style.danger
                        font.pixelSize: Style.fontSizeSmall
                        wrapMode: Text.WordWrap
                    }

                    MacButton {
                        text: qsTr("Retry")
                        primary: true
                        onClicked: if (root.controller) {
                            root.controller.selectStrategy(root.controller.selectedStrategyId)
                        }
                    }
                }

                Text {
                    visible: root.paramsModel ? root.paramsModel.pending : false
                    text: qsTr("Loading…")
                    color: Style.secondaryText
                    font.pixelSize: Style.fontSizeSmall
                }

                Column {
                    id: paramsGrid
                    Layout.fillWidth: true
                    visible: root.paramsModel ? root.paramsModel.present : false
                    spacing: 4

                    Repeater {
                        model: root.paramsModel ? root.paramsModel.params : []

                        delegate: Row {
                            required property var modelData
                            width: paramsGrid.width
                            spacing: Style.gap

                            Text {
                                width: 200
                                text: root.paramLabel(modelData.key)
                                color: Style.secondaryText
                                font.pixelSize: 12
                                elide: Text.ElideRight
                            }

                            Text {
                                text: modelData.value === undefined
                                      ? "—" : String(modelData.value)
                                color: Style.text
                                font.pixelSize: 12
                            }
                        }
                    }
                }

                Text {
                    visible: root.paramsModel ? root.paramsModel.present : false
                    Layout.topMargin: Style.gapSmall
                    text: qsTr("Read-only view. Changing parameters is not available in this version.")
                    color: Style.tertiaryText
                    font.pixelSize: Style.fontSizeSmall
                    wrapMode: Text.WordWrap
                }
            }
        }

        // -- Risk & execution ------------------------------------------------
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: riskColumn.implicitHeight + 2 * Style.pad
            radius: Style.radius
            color: Style.contentBackground
            border.width: 1
            border.color: Style.separator

            ColumnLayout {
                id: riskColumn

                anchors.fill: parent
                anchors.margins: Style.pad
                spacing: Style.gapSmall

                Text {
                    text: qsTr("Risk & execution")
                    color: Style.secondaryText
                    font.pixelSize: Style.fontSizeSmall
                }

                GridLayout {
                    Layout.fillWidth: true
                    columns: 4
                    columnSpacing: Style.gap
                    rowSpacing: Style.gap

                    MetricTile {
                        Layout.fillWidth: true
                        label: qsTr("Trading halted")
                        // get_status owns this badge; get_risk carries the same
                        // flag and two sources for one badge invite drift.
                        value: root.yesNo(root.controller ? root.controller.tradingHalted : false)
                        present: root.controller !== null && root.controller !== undefined
                        valueColor: root.controller && root.controller.tradingHalted
                                    ? Style.danger : Style.success
                    }

                    MetricTile {
                        Layout.fillWidth: true
                        label: qsTr("Daily drawdown")
                        value: root.fmtPct(root.risk ? root.risk.dailyDrawdown : NaN)
                        present: root.risk ? root.risk.present : false
                    }

                    MetricTile {
                        Layout.fillWidth: true
                        label: qsTr("Max drawdown")
                        value: root.fmtPct(root.risk ? root.risk.maxDrawdown : NaN)
                        present: root.risk ? root.risk.present : false
                    }

                    MetricTile {
                        Layout.fillWidth: true
                        label: qsTr("Rate limiter")
                        // No capacity is published, so never render "x / y".
                        value: root.risk ? String(root.risk.rateLimiterTokens) : "—"
                        present: root.risk ? root.risk.present : false
                        valueColor: root.risk && root.risk.rateLimiterExhausted
                                    ? Style.warning : Style.text
                    }
                }

                Text {
                    visible: root.risk ? root.risk.rateLimiterExhausted : false
                    text: qsTr("Order rate limiter exhausted")
                    color: Style.warning
                    font.pixelSize: Style.fontSizeSmall
                }

                GridLayout {
                    Layout.fillWidth: true
                    visible: root.risk ? root.risk.portfolioPresent : false
                    columns: 2
                    columnSpacing: Style.gap
                    rowSpacing: 4

                    Text { text: qsTr("Open positions"); color: Style.secondaryText; font.pixelSize: 12 }
                    Text {
                        text: root.risk ? String(root.risk.portfolioOpenPositions) : "—"
                        color: Style.text
                        font.pixelSize: 12
                    }

                    Text { text: qsTr("Total notional"); color: Style.secondaryText; font.pixelSize: 12 }
                    Text {
                        text: root.risk ? root.fmtMoney(root.risk.portfolioNotional) : "—"
                        color: Style.text
                        font.pixelSize: 12
                    }

                    Text { text: qsTr("Unrealized PnL"); color: Style.secondaryText; font.pixelSize: 12 }
                    Text {
                        text: root.risk ? root.fmtMoney(root.risk.portfolioUnrealizedPnl) : "—"
                        color: root.risk && root.risk.portfolioUnrealizedPnl < 0
                               ? Style.danger : Style.success
                        font.pixelSize: 12
                    }

                    Text { text: qsTr("Net exposure"); color: Style.secondaryText; font.pixelSize: 12 }
                    Text {
                        text: root.risk ? root.fmtMoney(root.risk.portfolioNetExposure) : "—"
                        color: Style.text
                        font.pixelSize: 12
                    }

                    Text { text: qsTr("Halt reason"); color: Style.secondaryText; font.pixelSize: 12 }
                    Text {
                        text: root.risk && root.risk.present
                              ? root.haltReasonLabel(root.risk.haltReasonCode) : "—"
                        color: Style.text
                        font.pixelSize: 12
                    }
                }

                Text {
                    Layout.fillWidth: true
                    visible: root.controller
                             && root.controller.connectionState !== EngineController.Ready
                    text: qsTr("Last known values — engine not connected.")
                    color: Style.warning
                    font.pixelSize: Style.fontSizeSmall
                }
            }
        }

        Item { Layout.fillHeight: true }
    }
}