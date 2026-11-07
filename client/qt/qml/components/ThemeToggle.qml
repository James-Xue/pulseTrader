import QtQuick
import Pulse.Client

/// Sun/moon toggle for the colour scheme, sitting in the title bar.
///
/// The glyphs are drawn with Canvas rather than a font character: the installed
/// fonts do not reliably carry U+2600/U+263E, and a missing glyph would leave an
/// empty button. The icon shows the mode the click will switch TO.
Item {
    id: root

    implicitWidth: 28
    implicitHeight: 28

    Rectangle {
        anchors.fill: parent
        radius: width / 2
        color: hover.containsMouse ? Style.controlBackgroundHover : "transparent"

        Canvas {
            id: glyph
            anchors.centerIn: parent
            width: 18
            height: 18

            Connections {
                target: Style
                function onDarkChanged() { glyph.requestPaint() }
            }

            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                ctx.strokeStyle = Style.secondaryText
                ctx.fillStyle = Style.secondaryText
                ctx.lineWidth = 1.4

                if (Style.dark) {
                    // Dark mode is on, so offer the sun: a filled disc plus
                    // eight rays.
                    ctx.beginPath()
                    ctx.arc(9, 9, 3.2, 0, Math.PI * 2)
                    ctx.fill()
                    for (let i = 0; i < 8; ++i) {
                        const a = i * Math.PI / 4
                        ctx.beginPath()
                        ctx.moveTo(9 + Math.cos(a) * 5.4, 9 + Math.sin(a) * 5.4)
                        ctx.lineTo(9 + Math.cos(a) * 7.6, 9 + Math.sin(a) * 7.6)
                        ctx.stroke()
                    }
                } else {
                    // Light mode is on, so offer the moon: a disc with a second
                    // disc punched out to leave a crescent.
                    ctx.beginPath()
                    ctx.arc(9, 9, 6.2, 0, Math.PI * 2)
                    ctx.fill()
                    ctx.globalCompositeOperation = "destination-out"
                    ctx.beginPath()
                    ctx.arc(12.2, 6.2, 5.6, 0, Math.PI * 2)
                    ctx.fill()
                    ctx.globalCompositeOperation = "source-over"
                }
            }
        }

        MouseArea {
            id: hover
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: Style.dark = !Style.dark
        }
    }
}
