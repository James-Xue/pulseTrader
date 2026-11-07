import QtQuick
import Pulse.Client

/// One strategy signal. Takes plain properties rather than a model object so it
/// can be reused outside a list (for example in a future detail popup).
Rectangle {
    id: root

    property string source: ""
    property string signalType: ""
    property real confidence: 0
    property string reason: ""
    property string publishedAt: ""

    implicitHeight: 76
    radius: Style.radius
    color: Style.surface
    border.color: Style.border

    Column {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 4

        Row {
            spacing: 8

            Text {
                text: root.source
                color: Style.text
                font.pixelSize: Style.fontSize
                font.bold: true
            }

            Rectangle {
                width: typeLabel.implicitWidth + 12
                height: 18
                radius: 4
                color: {
                    if (root.signalType === "Buy") {
                        return Style.good
                    }
                    if (root.signalType === "Sell") {
                        return Style.bad
                    }
                    return Style.surfaceAlt
                }

                Text {
                    id: typeLabel
                    anchors.centerIn: parent
                    text: root.signalType
                    color: "white"
                    font.pixelSize: 10
                }
            }

            Text {
                text: qsTr("conf %1").arg(root.confidence.toFixed(2))
                color: Style.muted
                font.pixelSize: 11
            }
        }

        Text {
            width: parent.width
            text: root.reason
            color: Style.muted
            font.pixelSize: 11
            elide: Text.ElideRight
        }

        Text {
            text: root.publishedAt
            color: Style.muted
            font.pixelSize: 10
        }
    }
}
