import QtQuick
import QtQuick.Controls
import Pulse.Client

/// macOS-style checkbox: a rounded 16 px box that fills with the accent colour
/// and draws a check mark when selected.
///
/// The tick is drawn with Canvas instead of a "✓" character, because the
/// installed fonts do not reliably carry U+2713.
CheckBox {
    id: root

    spacing: 8
    font.pixelSize: Style.fontSize

    contentItem: Text {
        text: root.text
        color: Style.text
        font: root.font
        leftPadding: root.indicator.width + root.spacing
        verticalAlignment: Text.AlignVCenter
        opacity: root.enabled ? 1.0 : 0.5
    }

    indicator: Rectangle {
        implicitWidth: 16
        implicitHeight: 16
        x: 0
        y: parent.height / 2 - height / 2
        radius: 4
        color: root.checked ? Style.accent : Style.controlBackground
        border.width: 1
        border.color: root.checked ? Style.accent : Style.controlBorder

        Canvas {
            id: tick
            anchors.fill: parent
            visible: root.checked

            Connections {
                target: root
                function onCheckedChanged() { tick.requestPaint() }
            }

            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                ctx.strokeStyle = "white"
                ctx.lineWidth = 2
                ctx.lineCap = "round"
                ctx.lineJoin = "round"
                ctx.beginPath()
                ctx.moveTo(4, 8.5)
                ctx.lineTo(7, 11.5)
                ctx.lineTo(12.5, 5)
                ctx.stroke()
            }
        }
    }
}
