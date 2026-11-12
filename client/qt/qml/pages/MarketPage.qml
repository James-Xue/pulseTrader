import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Pulse.Client

/// Live market view: one symbol, one sample per second, 10-second candles.
///
/// Data path: MarketStream (C++) polls the engine's get_market method at 1 Hz,
/// folds the tickers into CandleAggregator buckets and exposes the candle list
/// as a QVariantList. The chart is drawn locally with Canvas, so the client
/// stays free of charting dependencies.
///
/// Layout note: the toolbar is deliberately narrow (symbol picker, market
/// picker, period selector, Start/Stop) and the live price block lives inside
/// the chart card. The page also has to fit the 860 px minimum window, where
/// the page slot is only 651 px wide; a single row holding the price block
/// would push the controls off the right edge.
ColumnLayout {
    id: root

    property var controller
    property bool userStopped: false
    property bool showRaw: false

    /// The stream is owned by EngineController so it shares the control-socket
    /// endpoint and token without duplicating the login form state.
    readonly property var stream: root.controller ? root.controller.marketStream : null

    /// Re-evaluated whenever the C++ candlesChanged signal fires.
    readonly property var candleData: (root.stream && root.stream.candles)
                                      ? root.stream.candles : []
    readonly property int candleCount: root.candleData.length

    readonly property string lastSampleText:
        (root.stream && root.stream.lastSampleMs > 0)
        ? Qt.formatTime(new Date(root.stream.lastSampleMs), "hh:mm:ss")
        : "—"

    spacing: 0

    // -- helpers ------------------------------------------------------------

    /// Price formatting that keeps four decimals for liquid crypto pairs and
    /// two for index-sized values, without a third-party formatter.
    function fmtPrice(value) {
        if (!isFinite(value) || value <= 0) {
            return "—"
        }
        if (value >= 1000) {
            return value.toFixed(2)
        }
        if (value >= 100) {
            return value.toFixed(3)
        }
        if (value >= 1) {
            return value.toFixed(4)
        }
        return value.toFixed(6)
    }

    function effectiveSymbol() {
        const typed = symbolField.text.trim()
        if (typed.length > 0) {
            return typed
        }
        return symbolBox.currentText ? symbolBox.currentText : ""
    }

    /// "auto" means "let the engine pick its active market", which is what an
    /// empty market_type does on the wire.
    function marketTypeValue() {
        return marketBox.currentIndex === 0 ? "" : marketBox.currentText
    }

    function startStream() {
        if (root.controller && !root.controller.pollingEnabled) {
            return
        }
        root.userStopped = false
        const symbol = root.effectiveSymbol()
        if (root.stream && symbol.length > 0) {
            root.stream.start(symbol, root.marketTypeValue())
        }
    }

    function stopStream() {
        root.userStopped = true
        if (root.stream) {
            root.stream.stop()
        }
    }

    /// Auto-start only when the page is actually on screen, the engine is
    /// Ready, polling is enabled, and the user has not pressed Stop on this
    /// visit.
    function maybeAutoStart() {
        if (!root.visible || root.userStopped || !root.stream || !root.controller) {
            return
        }
        if (!root.controller.pollingEnabled) {
            return
        }
        if (root.controller.connectionState !== EngineController.Ready) {
            return
        }
        if (root.stream.streaming) {
            return
        }
        const symbol = root.effectiveSymbol()
        if (symbol.length === 0) {
            return
        }
        root.stream.start(symbol, root.marketTypeValue())
    }

    // -- toolbar ------------------------------------------------------------

    RowLayout {
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        Layout.margins: Style.pad
        spacing: Style.gap

        MacComboBox {
            id: symbolBox
            Layout.preferredWidth: 150
            Layout.minimumWidth: 0
            model: root.controller ? root.controller.symbols : []

            onActivated: {
                // The combo is now the source of truth; a stale typed symbol
                // must not silently override every later dropdown choice.
                symbolField.text = ""
                if (root.stream && root.stream.streaming) {
                    root.startStream()
                }
            }
        }

        MacTextField {
            id: symbolField
            Layout.preferredWidth: 120
            Layout.minimumWidth: 0
            placeholderText: qsTr("or type a symbol")

            onAccepted: {
                if (root.stream && root.stream.streaming) {
                    root.startStream()
                }
            }
        }

        MacComboBox {
            id: marketBox
            Layout.preferredWidth: 92
            Layout.minimumWidth: 0
            model: [qsTr("auto"), "spot", "futures", "cfd"]

            onActivated: {
                if (root.stream && root.stream.streaming) {
                    root.startStream()
                }
            }
        }

        MacSegmentedControl {
            id: periodControl
            Layout.preferredWidth: 126
            Layout.minimumWidth: 0
            items: ["1s", "5s", "10s"]
            currentIndex: root.stream
                          ? (root.stream.candlePeriodSec === 1 ? 0
                             : root.stream.candlePeriodSec === 5 ? 1 : 2)
                          : 2

            onActivated: function (index) {
                if (root.stream) {
                    root.stream.candlePeriodSec = [1, 5, 10][index]
                }
            }
        }

        MacButton {
            text: (root.stream && root.stream.streaming) ? qsTr("Stop") : qsTr("Start")
            primary: !(root.stream && root.stream.streaming)
            enabled: root.controller
                     && root.controller.connectionState === EngineController.Ready
                     && root.controller.pollingEnabled
            onClicked: {
                if (root.stream && root.stream.streaming) {
                    root.stopStream()
                } else {
                    root.startStream()
                }
            }
        }

        Item { Layout.fillWidth: true }
    }

    // -- chart --------------------------------------------------------------

    Rectangle {
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.leftMargin: Style.pad
        Layout.rightMargin: Style.pad
        radius: Style.radius
        color: Style.contentBackground
        border.width: 1
        border.color: Style.separator
        clip: true

        Canvas {
            id: chart
            anchors.fill: parent
            anchors.margins: 1

            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()

                const w = width
                const h = height
                if (w <= 1 || h <= 1) {
                    return
                }

                const candles = root.candleData
                const lastPrice = root.stream ? root.stream.lastPrice : 0

                // Margins leave room for the price axis on the right, the
                // time labels at the bottom, and the live-price overlay at
                // the top-left.
                const padLeft = 10
                const padRight = 74
                const padTop = 64
                const padBottom = 26
                const plotWidth = Math.max(1, w - padLeft - padRight)
                const plotHeight = Math.max(1, h - padTop - padBottom)
                const gridColor = Style.dark ? "#2C2C2E" : "#E8E8EA"
                const axisColor = Style.secondaryText

                ctx.font = "11px 'Noto Sans'"
                ctx.textAlign = "right"
                ctx.textBaseline = "middle"

                if (candles.length === 0) {
                    return
                }

                // 1. Price range, padded so the newest wick never touches the
                //    card edge. The last price participates even before its
                //    candle closes.
                let lo = Infinity
                let hi = -Infinity
                for (let i = 0; i < candles.length; ++i) {
                    const candle = candles[i]
                    if (candle.l < lo) {
                        lo = candle.l
                    }
                    if (candle.h > hi) {
                        hi = candle.h
                    }
                }
                if (lastPrice > 0) {
                    lo = Math.min(lo, lastPrice)
                    hi = Math.max(hi, lastPrice)
                }
                if (!isFinite(lo) || !isFinite(hi)) {
                    return
                }
                if (hi <= lo) {
                    const epsilon = Math.max(hi * 0.0005, 0.0001)
                    lo -= epsilon
                    hi += epsilon
                }
                const padding = (hi - lo) * 0.08
                lo -= padding
                hi += padding

                function yOf(price) {
                    return padTop + (hi - price) / (hi - lo) * plotHeight
                }

                // 2. Horizontal grid and price labels.
                const rows = 4
                for (let row = 0; row <= rows; ++row) {
                    const price = lo + (hi - lo) * row / rows
                    const y = yOf(price)
                    ctx.strokeStyle = gridColor
                    ctx.lineWidth = 1
                    ctx.beginPath()
                    ctx.moveTo(padLeft, y)
                    ctx.lineTo(w - padRight, y)
                    ctx.stroke()

                    ctx.fillStyle = axisColor
                    ctx.fillText(root.fmtPrice(price), w - 8, y)
                }

                // 3. Candles. Index-based x keeps the chart readable while the
                //    stream is still filling its first few bars.
                const slot = plotWidth / Math.max(1, candles.length)
                const bodyWidth = Math.max(2, Math.min(14, slot * 0.62))
                for (let i = 0; i < candles.length; ++i) {
                    const candle = candles[i]
                    const x = padLeft + (i + 0.5) * slot
                    const up = candle.c >= candle.o
                    const color = up ? Style.success : Style.danger

                    // The forming candle is translucent so the newest bar is
                    // visually distinguishable from closed history.
                    ctx.globalAlpha = candle.closed ? 1.0 : 0.72
                    ctx.strokeStyle = color
                    ctx.lineWidth = 1
                    ctx.beginPath()
                    ctx.moveTo(x, yOf(candle.h))
                    ctx.lineTo(x, yOf(candle.l))
                    ctx.stroke()

                    const openY = yOf(candle.o)
                    const closeY = yOf(candle.c)
                    const top = Math.min(openY, closeY)
                    const bodyHeight = Math.max(1, Math.abs(closeY - openY))
                    ctx.fillStyle = color
                    ctx.fillRect(x - bodyWidth / 2, top, bodyWidth, bodyHeight)
                }
                ctx.globalAlpha = 1.0

                // 4. Last price marker. The filled tag covers the grid label
                //    at the same height, so the two never overprint.
                if (lastPrice > 0) {
                    const y = yOf(lastPrice)
                    ctx.strokeStyle = Style.accent
                    ctx.lineWidth = 1
                    ctx.beginPath()
                    ctx.moveTo(padLeft, y)
                    ctx.lineTo(w - padRight, y)
                    ctx.stroke()

                    ctx.fillStyle = Style.accent
                    ctx.fillRect(w - padRight + 2, y - 8, padRight - 10, 16)
                    ctx.fillStyle = "white"
                    ctx.textAlign = "left"
                    ctx.textBaseline = "middle"
                    ctx.fillText(root.fmtPrice(lastPrice), w - padRight + 6, y)
                }

                // 5. Time labels: first and newest bucket.
                ctx.fillStyle = axisColor
                ctx.textBaseline = "top"
                ctx.textAlign = "left"
                ctx.fillText(Qt.formatTime(new Date(candles[0].t), "hh:mm:ss"),
                             padLeft, h - padBottom + 6)
                ctx.textAlign = "right"
                ctx.fillText(Qt.formatTime(new Date(candles[candles.length - 1].t),
                                           "hh:mm:ss"),
                             w - padRight, h - padBottom + 6)
            }

            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()
        }

        // Live price overlay. Keeping it inside the card is what lets the
        // toolbar fit the minimum window width.
        Column {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.margins: 12
            spacing: 0

            Text {
                text: root.fmtPrice(root.stream ? root.stream.lastPrice : 0)
                color: (root.stream && root.stream.changePct < 0)
                       ? Style.danger : Style.success
                font.pixelSize: 28
                font.bold: true
            }

            Row {
                spacing: Style.gap

                Text {
                    text: qsTr("bid %1").arg(root.fmtPrice(root.stream ? root.stream.bid : 0))
                    color: Style.secondaryText
                    font.pixelSize: Style.fontSizeSmall
                }
                Text {
                    text: qsTr("ask %1").arg(root.fmtPrice(root.stream ? root.stream.ask : 0))
                    color: Style.secondaryText
                    font.pixelSize: Style.fontSizeSmall
                }
                Text {
                    text: root.stream
                          ? ((root.stream.changePct >= 0 ? "+" : "")
                             + root.stream.changePct.toFixed(2) + "%")
                          : "—"
                    color: (root.stream && root.stream.changePct < 0)
                           ? Style.danger : Style.success
                    font.pixelSize: Style.fontSizeSmall
                }
            }
        }

        Text {
            anchors.centerIn: parent
            visible: root.candleCount === 0
            text: (root.stream && root.stream.streaming)
                  ? qsTr("Collecting the first 1-second samples…")
                  : qsTr("Choose a symbol and press Start")
            color: Style.tertiaryText
            font.pixelSize: Style.fontSize
        }

        Text {
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: 12
            visible: root.stream && root.stream.streaming
            text: (root.stream && root.stream.engineReady && !root.stream.stale)
                  ? "● LIVE" : "● WAITING"
            color: (root.stream && root.stream.engineReady && !root.stream.stale)
                   ? Style.success : Style.warning
            font.pixelSize: 10
            font.bold: true
        }
    }

    // -- footer -------------------------------------------------------------

    RowLayout {
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        Layout.leftMargin: Style.pad
        Layout.rightMargin: Style.pad
        Layout.bottomMargin: Style.pad
        spacing: Style.gap

        Text {
            text: qsTr("Samples %1").arg(root.stream ? root.stream.sampleCount : 0)
            color: Style.secondaryText
            font.pixelSize: Style.fontSizeSmall
        }
        Text {
            text: qsTr("Candles %1").arg(root.candleCount)
            color: Style.secondaryText
            font.pixelSize: Style.fontSizeSmall
        }
        Text {
            text: qsTr("Period %1s").arg(root.stream ? root.stream.candlePeriodSec : 10)
            color: Style.secondaryText
            font.pixelSize: Style.fontSizeSmall
        }
        Text {
            text: qsTr("Last %1").arg(root.lastSampleText)
            color: Style.secondaryText
            font.pixelSize: Style.fontSizeSmall
        }

        StaleBadge {
            visible: root.stream && root.stream.streaming && root.stream.stale
        }

        Text {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            text: root.stream ? root.stream.lastError : ""
            color: Style.danger
            font.pixelSize: Style.fontSizeSmall
            horizontalAlignment: Text.AlignRight
            elide: Text.ElideRight
        }

        MacSwitch {
            text: qsTr("Raw")
            checked: root.showRaw
            onToggled: root.showRaw = checked
        }
    }

    // -- optional raw ticker dump, kept for debugging -----------------------

    Rectangle {
        visible: root.showRaw
        Layout.preferredHeight: root.showRaw ? 140 : 0
        Layout.fillWidth: true
        Layout.leftMargin: Style.pad
        Layout.rightMargin: Style.pad
        Layout.bottomMargin: Style.pad
        color: Style.controlBackground
        radius: Style.radiusSmall
        border.width: 1
        border.color: Style.separator
        clip: true

        Flickable {
            anchors.fill: parent
            anchors.margins: 8
            contentWidth: rawText.implicitWidth
            contentHeight: rawText.implicitHeight
            clip: true

            Text {
                id: rawText
                text: root.stream ? JSON.stringify(root.stream.rawTicker, null, 2) : ""
                color: Style.text
                font.family: "monospace"
                font.pixelSize: 11
            }
        }
    }

    // -- lifecycle ----------------------------------------------------------

    Component.onCompleted: root.maybeAutoStart()

    onVisibleChanged: {
        if (root.visible) {
            root.maybeAutoStart()
        } else if (root.stream && root.stream.streaming) {
            // Leaving the page is not a user "Stop": coming back resumes and
            // keeps the collected candles.
            root.stream.stop()
        }
    }

    Connections {
        target: root.controller
        function onConnectionStateChanged() { root.maybeAutoStart() }
        function onStatusChanged() { root.maybeAutoStart() }
        function onPollingEnabledChanged() {
            if (!root.controller) {
                return
            }
            if (root.controller.pollingEnabled) {
                root.maybeAutoStart()
            } else if (root.stream) {
                root.stream.stop()
            }
        }
    }

    Connections {
        target: root.stream
        function onCandlesChanged() { chart.requestPaint() }
        function onTickerChanged() { chart.requestPaint() }
        function onStreamingChanged() { chart.requestPaint() }
        function onStaleChanged() { chart.requestPaint() }
    }

    Connections {
        target: Style
        function onDarkChanged() { chart.requestPaint() }
    }
}