import QtQuick
import QtQuick.Controls
import Pulse.Client

/// macOS-style combo box: rounded bordered field with a chevron.
///
/// Only the closed control is restyled; the popup keeps the Basic delegate,
/// which already follows the window palette.
ComboBox {
    id: root

    implicitHeight: 28
    font.pixelSize: Style.fontSize

    contentItem: Text {
        leftPadding: 8
        rightPadding: root.indicator.width + 6
        text: root.displayText
        color: Style.text
        font: root.font
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        radius: Style.radiusSmall
        color: root.pressed ? Style.controlBackgroundPressed
                            : root.hovered ? Style.controlBackgroundHover
                                           : Style.controlBackground
        border.width: 1
        border.color: root.activeFocus ? Style.accent : Style.controlBorder
        opacity: root.enabled ? 1.0 : 0.45
    }

    indicator: Canvas {
        x: root.width - width - 9
        y: root.height / 2 - height / 2
        width: 10
        height: 10

        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            ctx.strokeStyle = Style.secondaryText
            ctx.lineWidth = 1.6
            ctx.lineCap = "round"
            ctx.lineJoin = "round"
            ctx.beginPath()
            ctx.moveTo(1, 3)
            ctx.lineTo(5, 7)
            ctx.lineTo(9, 3)
            ctx.stroke()
        }
    }
}
