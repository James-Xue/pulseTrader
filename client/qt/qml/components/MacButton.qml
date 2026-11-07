import QtQuick
import QtQuick.Controls
import Pulse.Client

/// macOS-style push button.
///
/// `primary` switches to the filled blue variant used for the default action;
/// otherwise it is the light bordered button. Only the background and content
/// are replaced, so `enabled`, `text`, `onClicked` and Layout attachment behave
/// exactly as on a plain Button.
Button {
    id: root

    /// Filled accent variant for the default action in a dialog.
    property bool primary: false

    implicitHeight: 28
    leftPadding: 12
    rightPadding: 12
    font.pixelSize: Style.fontSize

    contentItem: Text {
        text: root.text
        color: root.primary ? "white" : Style.text
        font: root.font
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        radius: Style.radiusSmall
        color: {
            if (root.primary) {
                return root.pressed ? Style.accentPressed : Style.accent
            }
            if (root.pressed) {
                return Style.controlBackgroundPressed
            }
            return root.hovered ? Style.controlBackgroundHover
                                : Style.controlBackground
        }
        border.width: root.primary ? 0 : 1
        border.color: Style.controlBorder
        opacity: root.enabled ? 1.0 : 0.45
    }
}
