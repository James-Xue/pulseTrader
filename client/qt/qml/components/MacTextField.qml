import QtQuick
import QtQuick.Controls
import Pulse.Client

/// Text field in the macOS idiom: light rounded fill, hairline border, and a
/// blue border while focused.
///
/// Wraps the Basic-style TextField and replaces only its background, so
/// validation, echo modes, input hints and Layout attachment all behave as
/// they would on a plain TextField.
TextField {
    id: root

    implicitHeight: 28
    leftPadding: 8
    rightPadding: 8
    topPadding: 4
    bottomPadding: 4

    color: Style.text
    placeholderTextColor: Style.tertiaryText
    selectionColor: Style.accent
    selectedTextColor: "white"
    font.pixelSize: Style.fontSize

    background: Rectangle {
        radius: Style.radiusSmall
        color: root.enabled ? Style.controlBackground
                            : Style.controlBackgroundPressed
        border.width: 1
        border.color: root.activeFocus ? Style.accent : Style.controlBorder
    }
}
