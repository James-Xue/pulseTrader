import QtQuick
import Pulse.Client

/// Small marker for data that is older than twice its poll interval.
Rectangle {
    id: root

    property string text: qsTr("stale")

    implicitWidth: label.implicitWidth + 12
    implicitHeight: 18
    radius: 4
    color: Style.surfaceAlt

    Text {
        id: label
        anchors.centerIn: parent
        text: root.text
        color: Style.warn
        font.pixelSize: 10
    }
}
