import QtQuick
import Pulse.Client

/// One label/value pair. `present` separates "the engine did not report this"
/// from "the value is zero", which matters for balances.
Rectangle {
    id: root

    property string label: ""
    property string value: "—"
    property bool present: true
    property color valueColor: Style.text

    implicitWidth: 180
    implicitHeight: 84
    radius: Style.radius
    color: Style.contentBackground
    border.color: Style.separator

    Column {
        anchors.fill: parent
        anchors.margins: Style.pad
        spacing: 6

        Text {
            text: root.label
            color: Style.secondaryText
            font.pixelSize: 11
        }

        Text {
            text: root.present ? root.value : qsTr("unknown")
            color: root.present ? root.valueColor : Style.secondaryText
            font.pixelSize: 20
            font.bold: true
            elide: Text.ElideRight
            width: parent.width
        }
    }
}
