import QtQuick
import QtQuick.Layouts
import Pulse.Client

/// Always-visible header: connection state, engine identity and the two
/// risk-relevant flags (halted, active market).
Rectangle {
    id: root

    property var controller

    implicitHeight: 48
    color: Style.surface

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: Style.pad
        anchors.rightMargin: Style.pad
        spacing: Style.gap

        Rectangle {
            width: 10
            height: 10
            radius: 5
            color: {
                if (!root.controller) {
                    return Style.muted
                }
                if (root.controller.connectionState === EngineController.Ready) {
                    return Style.good
                }
                if (root.controller.connectionState === EngineController.Connecting) {
                    return Style.warn
                }
                return Style.bad
            }
        }

        Text {
            text: root.controller
                  ? (root.controller.connectionState === EngineController.Ready
                     ? qsTr("connected") : qsTr("disconnected"))
                  : qsTr("no controller")
            color: Style.text
            font.pixelSize: Style.fontSize
        }

        Rectangle {
            visible: root.controller && root.controller.tradingHalted
            implicitWidth: haltedLabel.implicitWidth + 16
            implicitHeight: 22
            radius: 4
            color: Style.bad

            Text {
                id: haltedLabel
                anchors.centerIn: parent
                text: qsTr("TRADING HALTED")
                color: "white"
                font.pixelSize: 11
                font.bold: true
            }
        }

        Item { Layout.fillWidth: true }

        Text {
            text: root.controller
                  ? qsTr("v%1 · %2 · %3 · up %4s")
                        .arg(root.controller.engineVersion)
                        .arg(root.controller.network)
                        .arg(root.controller.activeMarket)
                        .arg(Math.round(root.controller.engineUptimeSec))
                  : qsTr("—")
            color: Style.muted
            font.pixelSize: 11
        }
    }
}
