import QtQuick
import Pulse.Client

/// Interface-language toggle, sitting in the title bar next to the theme one.
///
/// Like ThemeToggle, the glyph shows the language the click will switch TO:
/// "中" while the UI is English, "EN" while it is Chinese. Both glyphs are real
/// characters, so no font-completeness trick is needed here.
Item {
    id: root

    implicitWidth: 28
    implicitHeight: 28

    Rectangle {
        anchors.fill: parent
        radius: width / 2
        color: hover.containsMouse ? Style.controlBackgroundHover : "transparent"

        Text {
            anchors.centerIn: parent
            // The target language, not the current one: an English UI offers
            // "中", a Chinese UI offers "EN".
            text: Translator.language === "zh" ? "EN" : "中"
            color: Style.secondaryText
            font.pixelSize: Style.fontSizeSmall
            font.weight: Font.DemiBold
        }

        MouseArea {
            id: hover
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: Translator.language = Translator.language === "zh" ? "en" : "zh"
        }
    }
}