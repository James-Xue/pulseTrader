import QtQuick
import QtQuick.Controls
import Pulse.Client

/// macOS-style switch: a rounded track with a white knob that slides.
Switch {
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
        id: track

        implicitWidth: 38
        implicitHeight: 22
        x: 0
        y: parent.height / 2 - height / 2
        radius: height / 2
        color: root.checked ? Style.accent : Style.controlBackgroundPressed
        border.width: root.checked ? 0 : 1
        border.color: Style.controlBorder

        Rectangle {
            id: knob

            width: 18
            height: 18
            radius: 9
            y: (parent.height - height) / 2
            x: root.checked ? parent.width - width - 2 : 2
            color: "white"
            border.width: 1
            border.color: Qt.rgba(0.0, 0.0, 0.0, 0.10)

            Behavior on x {
                NumberAnimation { duration: 120; easing.type: Easing.OutCubic }
            }
        }
    }
}
