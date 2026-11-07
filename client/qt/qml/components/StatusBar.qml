import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Pulse.Client

/// Connection state and engine identity, rendered as one compact line.
///
/// It lives in the title bar now (macOS puts status in the chrome, not in a
/// second toolbar), so it is a Row, not a full-width bar.
Row {
    id: root

    property var controller

    spacing: 8

    Rectangle {
        width: 8
        height: 8
        radius: 4
        anchors.verticalCenter: parent.verticalCenter
        color: {
            if (!root.controller) {
                return Style.tertiaryText
            }
            if (root.controller.connectionState === EngineController.Ready) {
                return Style.success
            }
            if (root.controller.connectionState === EngineController.Connecting) {
                return Style.warning
            }
            return Style.danger
        }
    }

    Text {
        anchors.verticalCenter: parent.verticalCenter
        text: root.controller
              ? (root.controller.connectionState === EngineController.Ready
                 ? qsTr("connected") : qsTr("disconnected"))
              : qsTr("no controller")
        color: Style.secondaryText
        font.pixelSize: Style.fontSizeSmall
    }

    Rectangle {
        visible: root.controller && root.controller.tradingHalted
        width: haltedLabel.implicitWidth + 12
        height: 18
        radius: Style.radiusSmall
        anchors.verticalCenter: parent.verticalCenter
        color: Style.danger

        Text {
            id: haltedLabel
            anchors.centerIn: parent
            text: qsTr("HALTED")
            color: "white"
            font.pixelSize: 10
            font.bold: true
        }
    }

    Text {
        anchors.verticalCenter: parent.verticalCenter
        text: root.controller
              ? qsTr("v%1 · %2 · %3")
                    .arg(root.controller.engineVersion)
                    .arg(root.controller.network)
                    .arg(root.controller.activeMarket)
              : "—"
        color: Style.tertiaryText
        font.pixelSize: Style.fontSizeSmall
    }
}
