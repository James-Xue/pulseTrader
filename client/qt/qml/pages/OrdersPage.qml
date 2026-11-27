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

    /// History strategy filter. Empty = all rows. Keys are derived from the
    /// engine's `strategy` field: team entries collapse to
    /// `xau_team:<member>`, venue SL/TP exits to `paper_exit`, everything
    /// else keeps its engine strategy name.
    property string strategyFilter: ""

    function strategyKey(row) {
        const raw = String(row.strategy || row.client_order_id || "")
        if (raw.startsWith("xau_team:")) {
            const parts = raw.split(":")
            return parts.length >= 2 ? "xau_team:" + parts[1] : raw
        }
        if (raw.startsWith("paper_exit_")) {
            return "paper_exit"
        }
        return raw
    }

    function strategyLabel(key) {
        if (!key) return qsTr("All strategies")
        if (key.startsWith("xau_team:")) return key.substring(9)
        if (key === "paper_exit") return qsTr("Venue exits")
        return key
    }

    readonly property var strategyKeys: {
        const orders = root.controller ? root.controller.mergedTradeHistory : []
        const seen = {}
        for (let i = 0; i < orders.length; ++i) {
            const key = root.strategyKey(orders[i])
            if (key.length > 0) seen[key] = true
        }
        return Object.keys(seen).sort()
    }
    readonly property var strategyOptions: [""].concat(root.strategyKeys)

    /// History date filter, mirroring the controller ("yyyy-MM-dd", "" = all).
    readonly property string historyDate: root.controller
        ? root.controller.historyDate : ""

    function dateLabel(key) {
        if (!key) return qsTr("All dates")
        const day = new Date(key + "T00:00:00")
        return Qt.formatDate(day, "yyyy-MM-dd ddd")
    }

    /// "" (all dates) plus every day seen in the newest unfiltered fetch. A
    /// manually entered day is kept even when that page does not list it.
    readonly property var dateKeys: {
        const days = root.controller ? root.controller.historyDates : []
        const keys = [""].concat(days)
        if (root.historyDate !== "" && keys.indexOf(root.historyDate) < 0) {
            keys.push(root.historyDate)
        }
        return keys
    }
    readonly property var dateOptions: root.dateKeys.map(root.dateLabel)

    /// Manual date entry, for days older than anything the picker has seen.
    property string dateInput: ""
    property bool dateInputInvalid: false

    function applyDateInput() {
        if (!root.controller) return
        const value = root.dateInput.trim()
        const valid = /^\d{4}-\d{2}-\d{2}$/.test(value)
                      && !isNaN(Date.parse(value + "T00:00:00"))
        if (!valid) {
            root.dateInputInvalid = true
            return
        }
        root.dateInputInvalid = false
        root.controller.setHistoryDate(value)
    }

    /// A day that vanished from the list (e.g. a new day rolled over) must
    /// not leave the page silently filtered by an unselectable date.
    Connections {
        target: root.controller
        function onHistoryDatesChanged() {
            if (root.historyDate !== ""
                && root.dateKeys.indexOf(root.historyDate) < 0) {
                root.controller.setHistoryDate("")
            }
        }
    }

    onStrategyKeysChanged: {
        if (root.strategyFilter !== ""
            && root.strategyKeys.indexOf(root.strategyFilter) < 0) {
            root.strategyFilter = ""
        }
    }

    /// Which merged order is expanded (its `key`); "" = none.
    property string expandedKey: ""

    /// Trade-history fills folded into round-trip orders. Filtering happens on
    /// the order (the exit's day, else the entry's), so a buy and its sell
    /// never appear as two rows.
    readonly property var filteredHistory: {
        const orders = root.controller ? root.controller.mergedTradeHistory : []
        const out = []
        for (let i = 0; i < orders.length; ++i) {
            const order = orders[i]
            if (root.strategyFilter !== ""
                && root.strategyKey(order) !== root.strategyFilter) {
                continue
            }
            const when = Number(order.exit_time_ms)
                         || Number(order.entry_time_ms) || 0
            const day = Qt.formatDate(new Date(when), "yyyy-MM-dd")
            if (root.historyDate !== "" && day !== root.historyDate) {
                continue
            }
            // The engine's page has no date section role, so inject one for
            // the ListView section headers.
            const copy = Object.assign({}, order)
            copy.dateKey = day
            out.push(copy)
        }
        return out
    }

    /// Stats over the merged orders: counting fills would count every entry
    /// and every exit, so the trade count and the win rate were meaningless.
    readonly property var orderStats: {
        const orders = root.filteredHistory
        let closed = 0
        let wins = 0
        let net = 0
        let fees = 0
        for (let i = 0; i < orders.length; ++i) {
            fees += Number(orders[i].fees || 0)
            if (orders[i].status !== "closed") continue
            closed += 1
            net += Number(orders[i].pnl || 0)
            if (Number(orders[i].pnl || 0) > 0) wins += 1
        }
        return {
            count: orders.length,
            closed: closed,
            wins: wins,
            net: net,
            fees: fees,
            winRate: closed > 0 ? wins / closed : 0
        }
    }

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
    readonly property int historyCount: root.filteredHistory.length
    readonly property int historyTotal: root.controller
        ? root.controller.mergedTradeHistory.length : 0

    /// Fixed column widths, shared by the header and every row so they stay
    /// aligned; the table can be wider than the viewport and scroll sideways.
    readonly property var historyColumns: [150, 110, 60, 80, 100, 90, 80, 80, 140]
    readonly property real historyTableWidth: {
        let total = 0
        for (let i = 0; i < root.historyColumns.length; ++i)
            total += root.historyColumns[i]
        return total + Style.gapSmall * (root.historyColumns.length - 1)
    }

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

            SelectableText {
                text: qsTr("Orders")
                color: Style.text
                font.pixelSize: Style.titleSize
                font.weight: Font.DemiBold
            }

            SelectableText {
                text: qsTr("%1 active · %2 recent")
                      .arg(root.activeCount).arg(root.reportCount)
                color: Style.secondaryText
                font.pixelSize: Style.fontSizeSmall
            }

            Item { Layout.fillWidth: true }

            MacComboBox {
                visible: viewStack.currentIndex === 2
                Layout.preferredWidth: 200
                model: root.strategyOptions.map(function(key) {
                    return root.strategyLabel(key)
                })
                currentIndex: {
                    const index = root.strategyOptions.indexOf(root.strategyFilter)
                    return index >= 0 ? index : 0
                }
                onActivated: function(index) {
                    root.strategyFilter = root.strategyOptions[index] || ""
                }
            }

            // Date filter: one local calendar day at a time. The day list
            // comes from the newest unfiltered fetch, so picking a day does
            // not shrink the choices to that day.
            MacComboBox {
                visible: viewStack.currentIndex === 2
                Layout.preferredWidth: 180
                model: root.dateOptions
                currentIndex: Math.max(0, root.dateKeys.indexOf(root.historyDate))
                onActivated: function(index) {
                    if (!root.controller) return
                    root.controller.setHistoryDate(root.dateKeys[index] || "")
                }
            }

            MacButton {
                text: qsTr("Refresh")
                onClicked: {
                    if (!root.controller) return
                    root.controller.refreshAll()
                    if (viewStack.currentIndex === 2) {
                        root.controller.fetchTradeHistory()
                    }
                }
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
                Layout.preferredWidth: 360
                items: [qsTr("Active (%1)").arg(root.activeCount),
                        qsTr("Recent (%1)").arg(root.reportCount),
                        qsTr("History (%1)").arg(root.historyCount)]
                currentIndex: viewStack.currentIndex
                onActivated: function (index) {
                    viewStack.currentIndex = index
                    if (index === 2 && root.controller) {
                        root.controller.fetchTradeHistory()
                    }
                }
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

                SelectableText { text: qsTr("Symbol"); color: Style.tertiaryText; font.pixelSize: Style.fontSizeSmall; Layout.preferredWidth: 130 }
                SelectableText { text: qsTr("Side"); color: Style.tertiaryText; font.pixelSize: Style.fontSizeSmall; Layout.preferredWidth: 60 }
                SelectableText { text: qsTr("Type"); color: Style.tertiaryText; font.pixelSize: Style.fontSizeSmall; Layout.preferredWidth: 80 }
                SelectableText { text: qsTr("Filled / qty"); color: Style.tertiaryText; font.pixelSize: Style.fontSizeSmall; Layout.preferredWidth: 120 }
                SelectableText { text: qsTr("Status"); color: Style.tertiaryText; font.pixelSize: Style.fontSizeSmall; Layout.preferredWidth: 90 }
                SelectableText { text: qsTr("Market"); color: Style.tertiaryText; font.pixelSize: Style.fontSizeSmall; Layout.fillWidth: true }
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

                        SelectableText {
                            text: model.symbol === undefined ? "" : model.symbol
                            color: Style.text
                            font.pixelSize: 12
                            Layout.preferredWidth: 130
                        }
                        SelectableText {
                            text: model.side === undefined ? "" : model.side
                            // The engine serializes lowercase "buy" / "sell".
                            color: "buy" === model.side ? Style.success : Style.danger
                            font.pixelSize: 12
                            Layout.preferredWidth: 60
                        }
                        SelectableText {
                            text: model.type === undefined ? "" : model.type
                            color: Style.secondaryText
                            font.pixelSize: 12
                            Layout.preferredWidth: 80
                        }
                        SelectableText {
                            text: model.filled_qty === undefined ? ""
                                  : "%1 / %2".arg(Number(model.filled_qty))
                                                   .arg(Number(model.requested_qty))
                            color: Style.text
                            font.pixelSize: 12
                            Layout.preferredWidth: 120
                        }
                        SelectableText {
                            text: model.status === undefined ? "" : model.status
                            color: root.statusColor(model.status === undefined ? "" : model.status)
                            font.pixelSize: 11
                            Layout.preferredWidth: 90
                        }
                        SelectableText {
                            text: model.market_type === undefined ? "" : model.market_type
                            color: Style.tertiaryText
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
        }

        // -- Recent fills ---------------------------------------------------
        ColumnLayout {
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: Style.pad + 10
                Layout.rightMargin: Style.pad + 10
                spacing: Style.gap

                SelectableText { text: qsTr("Symbol"); color: Style.tertiaryText; font.pixelSize: Style.fontSizeSmall; Layout.preferredWidth: 130 }
                SelectableText { text: qsTr("Status"); color: Style.tertiaryText; font.pixelSize: Style.fontSizeSmall; Layout.preferredWidth: 90 }
                SelectableText { text: qsTr("Fill price"); color: Style.tertiaryText; font.pixelSize: Style.fontSizeSmall; Layout.preferredWidth: 110 }
                SelectableText { text: qsTr("Market"); color: Style.tertiaryText; font.pixelSize: Style.fontSizeSmall; Layout.preferredWidth: 90 }
                SelectableText { text: qsTr("Filled at"); color: Style.tertiaryText; font.pixelSize: Style.fontSizeSmall; Layout.fillWidth: true }
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

                        SelectableText {
                            text: model.symbol === undefined ? "" : model.symbol
                            color: Style.text
                            font.pixelSize: 12
                            Layout.preferredWidth: 130
                        }
                        SelectableText {
                            // ExecutionReport serializes `final_status`, not
                            // `status`.
                            text: model.final_status === undefined ? "" : model.final_status
                            color: root.statusColor(model.final_status === undefined
                                                    ? "" : model.final_status)
                            font.pixelSize: 12
                            Layout.preferredWidth: 90
                        }
                        SelectableText {
                            text: model.avg_fill_price === undefined ? ""
                                  : String(model.avg_fill_price)
                            color: Style.text
                            font.pixelSize: 12
                            Layout.preferredWidth: 110
                        }
                        SelectableText {
                            text: model.market_type === undefined ? "" : model.market_type
                            color: Style.tertiaryText
                            font.pixelSize: 11
                            Layout.preferredWidth: 90
                        }
                        SelectableText {
                            text: model.fill_time_str === undefined ? ""
                                  : model.fill_time_str
                            color: Style.secondaryText
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

        // -- Trade history --------------------------------------------------
        // The table the popup used to carry, now a page view: persisted fills
        // from get_trade_history with a summary line. The horizontal
        // scrollbar appears whenever the viewport is narrower than the table.
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Style.gapSmall

            SelectableText {
                Layout.fillWidth: true
                color: Style.secondaryText
                font.pixelSize: Style.fontSizeSmall
                wrapMode: Text.WordWrap
                text: {
                    const stats = root.orderStats
                    if (stats.count === 0) {
                        return qsTr("No trades yet — fills appear here once recorded")
                    }
                    if (root.historyDate !== "") {
                        return qsTr("Showing %1 orders on %2 · closed %3 · net %4 · fees %5")
                            .arg(stats.count)
                            .arg(root.dateLabel(root.historyDate))
                            .arg(stats.closed)
                            .arg(stats.net.toFixed(2))
                            .arg(stats.fees.toFixed(2))
                    }
                    if (root.strategyFilter !== "") {
                        return qsTr("Showing %1 of %2 orders for %3 · net %4 · fees %5")
                            .arg(stats.count)
                            .arg(root.historyTotal)
                            .arg(root.strategyLabel(root.strategyFilter))
                            .arg(stats.net.toFixed(2))
                            .arg(stats.fees.toFixed(2))
                    }
                    return qsTr("%1 orders · %2 closed · win rate %3% · net %4 · fees %5")
                        .arg(stats.count)
                        .arg(stats.closed)
                        .arg((stats.winRate * 100).toFixed(1))
                        .arg(stats.net.toFixed(2))
                        .arg(stats.fees.toFixed(2))
                }
            }

            // Manual date entry: the picker only lists days present in the
            // newest fetch, so an older day must be typed.
            RowLayout {
                Layout.fillWidth: true
                spacing: Style.gapSmall

                SelectableText {
                    text: qsTr("Jump to date")
                    color: Style.secondaryText
                    font.pixelSize: Style.fontSizeSmall
                }
                MacTextField {
                    Layout.preferredWidth: 130
                    placeholderText: qsTr("YYYY-MM-DD")
                    text: root.dateInput
                    onTextChanged: root.dateInput = text
                    onAccepted: root.applyDateInput()
                    color: root.dateInputInvalid ? Style.danger : Style.text
                }
                MacButton {
                    text: qsTr("Go")
                    onClicked: root.applyDateInput()
                }
                MacButton {
                    text: qsTr("All dates")
                    enabled: root.historyDate !== ""
                    onClicked: {
                        if (root.controller) root.controller.setHistoryDate("")
                        root.dateInput = ""
                        root.dateInputInvalid = false
                    }
                }
                Item { Layout.fillWidth: true }
            }

            // The list and the empty state share one plain Item: a ListView is
            // a Layout item with no intrinsic height, and an EmptyState (a
            // Text with anchors) placed directly in a ColumnLayout is
            // undefined behaviour. The wrapper gives both a real box and lets
            // the empty state centre over the viewport.
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                ListView {
                    id: historyList
                    anchors.fill: parent
                    clip: true
                boundsBehavior: Flickable.StopAtBounds
                flickableDirection: Flickable.HorizontalAndVerticalFlick
                contentWidth: root.historyTableWidth
                model: root.filteredHistory

                // Group by local calendar day. Section headers make the
                // per-day blocks obvious without leaving the table view.
                section.property: "dateKey"
                section.criteria: ViewSection.FullString
                section.delegate: Rectangle {
                    width: Math.max(historyList.width, root.historyTableWidth)
                    height: 28
                    color: Style.contentBackground

                    SelectableText {
                        anchors.left: parent.left
                        anchors.leftMargin: 4
                        anchors.verticalCenter: parent.verticalCenter
                        text: root.dateLabel(section)
                        color: Style.secondaryText
                        font.pixelSize: Style.fontSizeSmall
                        font.bold: true
                    }
                }

                ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AlwaysOn }
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                header: RowLayout {
                    width: Math.max(ListView.view.width, root.historyTableWidth)
                    spacing: Style.gapSmall
                    SelectableText { text: qsTr("Time"); color: Style.tertiaryText
                           font.pixelSize: Style.fontSizeSmall
                           Layout.preferredWidth: root.historyColumns[0] }
                    SelectableText { text: qsTr("Symbol"); color: Style.tertiaryText
                           font.pixelSize: Style.fontSizeSmall
                           Layout.preferredWidth: root.historyColumns[1] }
                    SelectableText { text: qsTr("Side"); color: Style.tertiaryText
                           font.pixelSize: Style.fontSizeSmall
                           Layout.preferredWidth: root.historyColumns[2] }
                    SelectableText { text: qsTr("Qty"); color: Style.tertiaryText
                           font.pixelSize: Style.fontSizeSmall
                           horizontalAlignment: Text.AlignRight
                           Layout.preferredWidth: root.historyColumns[3] }
                    SelectableText { text: qsTr("Price"); color: Style.tertiaryText
                           font.pixelSize: Style.fontSizeSmall
                           horizontalAlignment: Text.AlignRight
                           Layout.preferredWidth: root.historyColumns[4] }
                    SelectableText { text: qsTr("PnL"); color: Style.tertiaryText
                           font.pixelSize: Style.fontSizeSmall
                           horizontalAlignment: Text.AlignRight
                           Layout.preferredWidth: root.historyColumns[5] }
                    SelectableText { text: qsTr("Fees"); color: Style.tertiaryText
                           font.pixelSize: Style.fontSizeSmall
                           horizontalAlignment: Text.AlignRight
                           Layout.preferredWidth: root.historyColumns[6] }
                    SelectableText { text: qsTr("Status"); color: Style.tertiaryText
                           font.pixelSize: Style.fontSizeSmall
                           Layout.preferredWidth: root.historyColumns[7] }
                    SelectableText { text: qsTr("Strategy"); color: Style.tertiaryText
                           font.pixelSize: Style.fontSizeSmall
                           Layout.preferredWidth: root.historyColumns[8] }
                }

                // One row per ROUND TRIP. The fills behind it are paired in
                // C++ by exchange_position_id, so a buy and its sell are one
                // order; clicking expands the entry and exit legs.
                delegate: Rectangle {
                    width: Math.max(historyList.width, root.historyTableWidth)
                    height: orderExpanded
                        ? 30 + orderRow.order.legs.length * 24
                          + (orderRow.order.unpaired ? 22 : 0) + 6
                        : 30
                    color: orderExpanded ? Style.controlBackgroundHover
                                         : "transparent"
                    radius: orderExpanded ? Style.radius : 0

                    readonly property var order: modelData
                    readonly property bool orderExpanded:
                        root.expandedKey === String(modelData.key)

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 4
                        anchors.rightMargin: 4
                        spacing: 0

                        RowLayout {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 30
                            spacing: Style.gapSmall

                            SelectableText {
                                text: (orderRow.orderExpanded ? "▾ " : "▸ ")
                                    + new Date(Number(orderRow.order.exit_time_ms)
                                               || Number(orderRow.order.entry_time_ms) || 0)
                                          .toLocaleString(Qt.locale(), "MM-dd HH:mm:ss")
                                color: Style.text
                                font.pixelSize: Style.fontSizeSmall
                                font.family: "monospace"
                                Layout.preferredWidth: root.historyColumns[0]
                            }
                            SelectableText {
                                text: orderRow.order.symbol
                                color: Style.text
                                font.pixelSize: Style.fontSizeSmall
                                Layout.preferredWidth: root.historyColumns[1]
                            }
                            SelectableText {
                                text: String(orderRow.order.side) === "buy"
                                      ? qsTr("Buy") : qsTr("Sell")
                                color: String(orderRow.order.side) === "buy"
                                       ? Style.success : Style.danger
                                font.pixelSize: Style.fontSizeSmall
                                Layout.preferredWidth: root.historyColumns[2]
                            }
                            SelectableText {
                                text: Number(orderRow.order.quantity || 0).toFixed(4)
                                color: Style.text
                                font.pixelSize: Style.fontSizeSmall
                                font.family: "monospace"
                                horizontalAlignment: Text.AlignRight
                                Layout.preferredWidth: root.historyColumns[3]
                            }
                            SelectableText {
                                text: Number(orderRow.order.status === "closed"
                                              ? orderRow.order.exit_price
                                              : orderRow.order.entry_price).toFixed(2)
                                color: Style.text
                                font.pixelSize: Style.fontSizeSmall
                                font.family: "monospace"
                                horizontalAlignment: Text.AlignRight
                                Layout.preferredWidth: root.historyColumns[4]
                            }
                            SelectableText {
                                text: Number(orderRow.order.pnl || 0).toFixed(2)
                                color: Number(orderRow.order.pnl || 0) >= 0
                                       ? Style.upColor : Style.downColor
                                font.pixelSize: Style.fontSizeSmall
                                font.family: "monospace"
                                horizontalAlignment: Text.AlignRight
                                Layout.preferredWidth: root.historyColumns[5]
                            }
                            SelectableText {
                                text: Number(orderRow.order.fees || 0).toFixed(2)
                                color: Style.secondaryText
                                font.pixelSize: Style.fontSizeSmall
                                font.family: "monospace"
                                horizontalAlignment: Text.AlignRight
                                Layout.preferredWidth: root.historyColumns[6]
                            }
                            SelectableText {
                                text: orderRow.order.unpaired
                                      ? qsTr("unpaired")
                                      : (orderRow.order.status === "closed"
                                         ? qsTr("closed") : qsTr("open"))
                                color: Style.secondaryText
                                font.pixelSize: Style.fontSizeSmall
                                Layout.preferredWidth: root.historyColumns[7]
                            }
                            SelectableText {
                                text: {
                                    const cid = String(orderRow.order.client_order_id || "")
                                    if (cid.indexOf("xau_team:") === 0) {
                                        const parts = cid.split(":")
                                        if (parts.length > 1 && parts[1] !== "") {
                                            return parts[1]
                                        }
                                    }
                                    return orderRow.order.strategy || cid || "—"
                                }
                                color: Style.tertiaryText
                                font.pixelSize: Style.fontSizeSmall
                                Layout.preferredWidth: root.historyColumns[8]
                            }
                        }

                        // The two legs, revealed on click: buy time/price and
                        // sell time/price (order is chronological, so the
                        // entry leg always comes first).
                        Repeater {
                            model: orderRow.order.legs

                            delegate: RowLayout {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 24
                                spacing: Style.gapSmall

                                SelectableText {
                                    text: String(modelData.side) === "buy"
                                          ? qsTr("Buy") : qsTr("Sell")
                                    color: String(modelData.side) === "buy"
                                           ? Style.success : Style.danger
                                    font.pixelSize: Style.fontSizeSmall
                                    font.bold: true
                                    Layout.preferredWidth: 48
                                }
                                SelectableText {
                                    text: new Date(Number(modelData.time_ms) || 0)
                                              .toLocaleString(Qt.locale(),
                                                              "yyyy-MM-dd HH:mm:ss")
                                    color: Style.secondaryText
                                    font.pixelSize: Style.fontSizeSmall
                                    font.family: "monospace"
                                    Layout.preferredWidth: 160
                                }
                                SelectableText {
                                    text: qsTr("price %1").arg(
                                              Number(modelData.price || 0).toFixed(2))
                                    color: Style.text
                                    font.pixelSize: Style.fontSizeSmall
                                    font.family: "monospace"
                                    Layout.preferredWidth: 110
                                }
                                SelectableText {
                                    text: "×" + Number(modelData.quantity || 0).toFixed(4)
                                    color: Style.text
                                    font.pixelSize: Style.fontSizeSmall
                                    font.family: "monospace"
                                    Layout.preferredWidth: 90
                                }
                                SelectableText {
                                    text: qsTr("fee %1").arg(
                                              Number(modelData.fees || 0).toFixed(2))
                                    color: Style.secondaryText
                                    font.pixelSize: Style.fontSizeSmall
                                    font.family: "monospace"
                                    Layout.preferredWidth: 80
                                }
                                SelectableText {
                                    visible: modelData.is_exit === true
                                    text: qsTr("PnL %1").arg(
                                              Number(modelData.pnl || 0).toFixed(2))
                                    color: Number(modelData.pnl || 0) >= 0
                                           ? Style.upColor : Style.downColor
                                    font.pixelSize: Style.fontSizeSmall
                                    font.family: "monospace"
                                    Layout.preferredWidth: 90
                                }
                                SelectableText {
                                    text: String(modelData.order_id || "")
                                    color: Style.tertiaryText
                                    font.pixelSize: Style.fontSizeSmall - 1
                                    font.family: "monospace"
                                    Layout.preferredWidth: 190
                                }
                            }
                        }

                        SelectableText {
                            Layout.fillWidth: true
                            visible: orderRow.order.unpaired === true
                            text: qsTr("Legacy fill without a venue id — cannot be paired")
                            color: Style.tertiaryText
                            font.pixelSize: Style.fontSizeSmall
                        }
                    }

                    // Declared last so it sits above the SelectableText cells
                    // (a TextEdit consumes clicks). Clicking toggles the legs
                    // instead of opening a separate dialog.
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            const key = String(modelData.key)
                            root.expandedKey =
                                (root.expandedKey === key) ? "" : key
                        }
                    }
                }

                }

                EmptyState {
                    anchors.centerIn: parent
                    visible: historyList.count === 0
                    text: qsTr("No trades yet")
                }
            }

            MacButton {
                Layout.alignment: Qt.AlignHCenter
                visible: root.controller
                         && root.controller.canLoadOlderHistory
                text: qsTr("Load older")
                onClicked: {
                    if (root.controller) root.controller.fetchOlderTradeHistory()
                }
            }
        }
    }

    // The history fetch is on-demand (tab click / Refresh). If the click lands
    // while the REST socket is still reconnecting, it is silently dropped —
    // retry once the connection becomes ready so the tab never stays empty.
    Connections {
        target: root.controller
        function onConnectionStateChanged() {
            if (viewStack.currentIndex === 2) {
                root.controller.fetchTradeHistory()
            }
        }
    }

}