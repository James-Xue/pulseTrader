import QtQuick
import QtQuick.Layouts
import Pulse.Client

/// Left navigation rail, the macOS "source list" shape: flat rows, a rounded
/// blue selection pill, no borders.
Rectangle {
    id: root

    property int currentIndex: 0
    property var items: [qsTr("Dashboard"), qsTr("Positions"), qsTr("Orders"),
                         qsTr("Signals"), qsTr("Market"), qsTr("Settings")]

    signal selected(int index)

    color: Style.sidebarBackground

    ColumnLayout {
        anchors.fill: parent
        anchors.topMargin: Style.gapSmall
        spacing: 1

        Repeater {
            model: root.items

            delegate: Rectangle {
                id: row

                Layout.fillWidth: true
                Layout.leftMargin: 10
                Layout.rightMargin: 10
                Layout.preferredHeight: 30
                radius: Style.radiusSmall
                color: {
                    if (index === root.currentIndex) {
                        return Style.selection
                    }
                    return hover.containsMouse ? Qt.rgba(0, 0, 0, 0.06) : "transparent"
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.leftMargin: 9
                    text: modelData
                    color: index === root.currentIndex ? "white" : Style.text
                    font.pixelSize: Style.fontSize
                    font.weight: index === root.currentIndex ? Font.DemiBold : Font.Normal
                }

                MouseArea {
                    id: hover
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.selected(index)
                }
            }
        }

        Item { Layout.fillHeight: true }
    }
}
