import QtQuick
import Pulse.Client

/// Transport-level failure banner. The last known data stays visible beneath
/// it; this bar only reports that it may be stale.
Rectangle {
    id: root

    property string message: ""

    implicitHeight: 32
    color: "#3a1f22"

    Text {
        anchors.fill: parent
        anchors.leftMargin: Style.pad
        anchors.rightMargin: Style.pad
        verticalAlignment: Text.AlignVCenter
        text: root.message
        color: Style.danger
        font.pixelSize: 12
        elide: Text.ElideRight
    }
}
