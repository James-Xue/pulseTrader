import QtQuick
import Pulse.Client

/// macOS segmented control: a rounded trough with a white selection pill that
/// slides between segments.
///
/// Used where a tab bar would be wrong for the platform (the Orders page has
/// two views of the same data, which is exactly what macOS segments are for).
Item {
    id: root

    /// Segment labels, in display order.
    property var items: []
    /// Currently selected segment.
    property int currentIndex: 0

    signal activated(int index)

    implicitWidth: 220
    implicitHeight: 26

    readonly property real segmentWidth:
        Math.max(1, root.items.length) > 0
        ? segments.width / Math.max(1, root.items.length)
        : segments.width

    Rectangle {
        anchors.fill: parent
        radius: Style.radiusSmall + 1
        color: Style.controlBackgroundPressed
        border.width: 1
        border.color: Style.controlBorder
    }

    Item {
        id: segments

        anchors.fill: parent
        anchors.margins: 2

        Rectangle {
            id: pill

            width: root.segmentWidth
            height: parent.height
            x: root.currentIndex * root.segmentWidth
            radius: Style.radiusSmall
            color: Style.controlBackground
            border.width: 1
            border.color: Style.separator

            Behavior on x {
                NumberAnimation { duration: 140; easing.type: Easing.OutCubic }
            }
        }

        Repeater {
            model: root.items

            delegate: Item {
                x: index * root.segmentWidth
                width: root.segmentWidth
                height: segments.height

                Text {
                    anchors.centerIn: parent
                    text: modelData
                    color: index === root.currentIndex ? Style.text
                                                       : Style.secondaryText
                    font.pixelSize: Style.fontSizeSmall
                    font.weight: index === root.currentIndex
                                 ? Font.DemiBold : Font.Normal
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.activated(index)
                }
            }
        }
    }
}
