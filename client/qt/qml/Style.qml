pragma Singleton

import QtQuick

/// Shared visual tokens. Pages must never hard-code a colour or spacing value;
/// everything comes from here so a theme change is a one-file edit.
QtObject {
    readonly property color bg: "#12151c"
    readonly property color surface: "#1b2029"
    readonly property color surfaceAlt: "#232a36"
    readonly property color border: "#2a3140"
    readonly property color text: "#e6e9ef"
    readonly property color muted: "#8b93a7"
    readonly property color accent: "#4c9aff"
    readonly property color good: "#3ecf8e"
    readonly property color warn: "#f2c94c"
    readonly property color bad: "#eb5757"

    readonly property int radius: 8
    readonly property int gap: 12
    readonly property int pad: 16
    readonly property int fontSize: 13
    readonly property int titleSize: 18
}
