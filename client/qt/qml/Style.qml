pragma Singleton

import QtQuick

/// macOS-like design tokens with a light and a dark variant.
///
/// The application runs on the Basic Controls style, which draws almost
/// nothing, so every colour, radius and type size in the UI comes from here.
/// `dark` is a normal (writable) property: the title-bar toggle flips it and
/// every binding re-evaluates, so the whole UI re-themes without a restart.
QtObject {
    /// Light is the default; Main.qml restores the saved choice at startup.
    property bool dark: false

    // -- Surfaces -----------------------------------------------------------
    readonly property color windowBackground: dark ? "#1E1E1E" : "#ECECEC"
    readonly property color sidebarBackground: dark ? "#252528" : "#E4E4E7"
    readonly property color contentBackground: dark ? "#1A1A1C" : "#FFFFFF"
    readonly property color controlBackground: dark ? "#2C2C2E" : "#FFFFFF"
    readonly property color controlBackgroundHover: dark ? "#3A3A3C" : "#F7F7F8"
    readonly property color controlBackgroundPressed: dark ? "#48484A" : "#E8E8EA"

    // -- Lines and text -----------------------------------------------------
    readonly property color separator: dark ? "#3A3A3C" : "#D1D1D6"
    readonly property color controlBorder: dark ? "#48484A" : "#C7C7CC"
    readonly property color text: dark ? "#F5F5F7" : "#1D1D1F"
    readonly property color secondaryText: dark ? "#A1A1A6" : "#6E6E73"
    readonly property color tertiaryText: dark ? "#7C7C82" : "#8E8E93"

    // -- Accents ------------------------------------------------------------
    readonly property color accent: "#0A84FF"
    readonly property color accentPressed: "#0069D9"
    readonly property color selection: "#0A84FF"
    readonly property color success: dark ? "#30D158" : "#30A46C"
    readonly property color warning: dark ? "#FFD60A" : "#E5A000"
    readonly property color danger: "#FF453A"

    // -- Shape --------------------------------------------------------------
    readonly property int radiusSmall: 5
    readonly property int radius: 8
    readonly property int radiusLarge: 12

    /// Outer window corner radius (macOS uses roughly 10-12 px).
    readonly property int windowRadius: 10
    readonly property int hairline: 1

    // -- Spacing ------------------------------------------------------------
    readonly property int gapSmall: 6
    readonly property int gap: 12
    readonly property int gapLarge: 20
    readonly property int pad: 16

    // -- Type ---------------------------------------------------------------
    // No SF Pro on Linux; Noto Sans is the closest neutral grotesque and Qt
    // falls back to Noto Sans CJK for Chinese glyphs automatically.
    readonly property string fontFamily: "Noto Sans"
    readonly property int fontSizeSmall: 11
    readonly property int fontSize: 13
    readonly property int fontSizeLarge: 15
    readonly property int titleSize: 20
    readonly property int sectionTitleSize: 13

    // -- Chrome -------------------------------------------------------------
    readonly property int titleBarHeight: 38
    readonly property int sidebarWidth: 208
    readonly property int rowHeight: 34
}
