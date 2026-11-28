import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Pulse.Client

/// Date-time field with a scroll-wheel picker.
///
/// The field is display-only: `epochMs` says what to show and `picked` carries
/// the value the trader confirms, so the owner keeps the filter state in one
/// place (the EngineController). Clicking the field opens five wheels —
/// year / month / day / hour / minute — and OK emits the composed instant in
/// local time.
///
/// `epochMs == 0` means "no bound": the field shows `placeholder` and the
/// wheels open on `fallbackMs` so they never start at the epoch.
Item {
    id: root

    /// Selected instant in epoch milliseconds; 0 = unset.
    property double epochMs: 0
    /// Where the wheels start when `epochMs` is unset (0 = now).
    property double fallbackMs: 0
    /// Shown in place of a date when no bound is selected.
    property string placeholder: qsTr("Any time")

    /// Emitted with the confirmed instant (epoch ms, local composition).
    signal picked(double epochMs)

    implicitWidth: 150
    implicitHeight: 28

    // The wheels cover 2020 onward: trade history cannot predate the project,
    // and a fixed base keeps the year column short enough to scroll.
    readonly property int yearBase: 2020
    readonly property int yearCount: Math.max(6, new Date().getFullYear()
                                                 - root.yearBase + 2)

    /// Days in the month the two left wheels currently show. Read by the day
    /// wheel's model, so shortening a month (or picking February) drops any
    /// now-invalid day index instead of rendering "31 Feb".
    readonly property int dayCount: {
        const year = root.yearBase + yearWheel.currentIndex
        const month = monthWheel.currentIndex + 1
        return new Date(year, month, 0).getDate()
    }

    // Shortening the month (or leaving February) can strand the day wheel on
    // an index its model no longer has; clamp it so OK can never compose a
    // date the calendar does not contain.
    onDayCountChanged: {
        if (dayWheel.currentIndex >= root.dayCount) {
            dayWheel.currentIndex = root.dayCount - 1
        }
    }

    function pad2(value) {
        return value < 10 ? "0" + value : String(value)
    }

    function displayText() {
        if (root.epochMs <= 0) {
            return root.placeholder
        }
        return Qt.formatDateTime(new Date(root.epochMs), "yyyy-MM-dd HH:mm")
    }

    /// Seed the wheels from the current value (or the fallback) and show them.
    function open() {
        const seed = root.epochMs > 0 ? new Date(root.epochMs)
                   : (root.fallbackMs > 0 ? new Date(root.fallbackMs) : new Date())
        yearWheel.currentIndex = Math.max(0, Math.min(
            root.yearCount - 1, seed.getFullYear() - root.yearBase))
        monthWheel.currentIndex = seed.getMonth()
        dayWheel.currentIndex = Math.max(0, Math.min(seed.getDate() - 1,
                                                     root.dayCount - 1))
        hourWheel.currentIndex = seed.getHours()
        minuteWheel.currentIndex = seed.getMinutes()
        popup.open()
    }

    /// Compose the wheel positions into one local instant and hand it up.
    function commit() {
        const picked = new Date(root.yearBase + yearWheel.currentIndex,
                                monthWheel.currentIndex,
                                dayWheel.currentIndex + 1,
                                hourWheel.currentIndex,
                                minuteWheel.currentIndex, 0, 0)
        root.picked(picked.getTime())
    }

    Rectangle {
        anchors.fill: parent
        radius: Style.radiusSmall
        color: hover.hovered ? Style.controlBackgroundHover
                             : Style.controlBackground
        border.width: 1
        border.color: (hover.hovered || popup.visible) ? Style.accent
                                                       : Style.controlBorder
    }

    Text {
        id: valueText
        anchors.left: parent.left
        anchors.leftMargin: 8
        anchors.right: chevron.left
        anchors.rightMargin: 4
        anchors.verticalCenter: parent.verticalCenter
        text: root.displayText()
        color: root.epochMs > 0 ? Style.text : Style.tertiaryText
        font.pixelSize: Style.fontSize
        elide: Text.ElideRight
    }

    SelectableText {
        id: chevron
        anchors.right: parent.right
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        text: "▾"
        color: Style.tertiaryText
        font.pixelSize: Style.fontSizeSmall
    }

    MouseArea {
        id: hover
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: root.open()
    }

    Popup {
        id: popup
        parent: root
        x: 0
        y: root.height + 4
        width: 340
        height: 236
        modal: true
        focus: true
        padding: Style.gap

        background: Rectangle {
            color: Style.controlBackground
            radius: Style.radius
            border.width: 1
            border.color: Style.controlBorder
        }

        contentItem: ColumnLayout {
            spacing: Style.gapSmall

            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0

                // 1. Year.
                Tumbler {
                    id: yearWheel
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    visibleItemCount: 5
                    wrap: true
                    model: root.yearCount
                    delegate: wheelDelegate
                }
                // 2. Month.
                Tumbler {
                    id: monthWheel
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    visibleItemCount: 5
                    wrap: true
                    model: 12
                    delegate: wheelDelegate
                }
                // 3. Day; the model shrinks with the selected month so an
                //    impossible date can never be confirmed.
                Tumbler {
                    id: dayWheel
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    visibleItemCount: 5
                    wrap: true
                    model: root.dayCount
                    delegate: wheelDelegate
                }
                // 4. Hour.
                Tumbler {
                    id: hourWheel
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    visibleItemCount: 5
                    wrap: true
                    model: 24
                    delegate: wheelDelegate
                }
                // 5. Minute.
                Tumbler {
                    id: minuteWheel
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    visibleItemCount: 5
                    wrap: true
                    model: 60
                    delegate: wheelDelegate
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Style.gapSmall

                Item { Layout.fillWidth: true }
                MacButton {
                    text: qsTr("Cancel")
                    onClicked: popup.close()
                }
                MacButton {
                    text: qsTr("OK")
                    onClicked: {
                        root.commit()
                        popup.close()
                    }
                }
            }
        }
    }

    /// Shared wheel row: grey out the values moving out of the band so the
    /// centred item reads as the selection.
    Component {
        id: wheelDelegate

        Text {
            required property int index

            // The year column is the only one that is neither 1-based nor
            // zero-padded: every other wheel shows its index as a two-digit
            // number (month/day are 1-based, hour/minute are not).
            readonly property var wheel: Tumbler.tumbler
            text: wheel === yearWheel ? String(root.yearBase + index)
                : wheel === monthWheel || wheel === dayWheel
                    ? root.pad2(index + 1)
                    : root.pad2(index)
            color: Style.text
            font.pixelSize: Style.fontSize
            font.family: "monospace"
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            opacity: 1.0 - Math.abs(Tumbler.displacement)
                            / (Tumbler.tumbler.visibleItemCount / 2)
        }
    }
}
