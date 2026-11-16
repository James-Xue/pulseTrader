import QtQuick
import Pulse.Client

/// The three macOS window controls.
///
/// The window is frameless (see Main.qml), so these carry the real behaviour:
/// minimize, maximize/restore, and close. Colours are the macOS traffic-light
/// values; the subtle border keeps the yellow button visible on a light title
/// bar. The operator chose the final mapping on 2026-11-16: the green button
/// minimizes, the yellow one maximizes, and red closes at the far right.
Row {
    id: root

    /// The ApplicationWindow these buttons control.
    property var target

    spacing: 8

    Repeater {
        model: [
            { base: "#28C840", hover: "#1DAD35", action: "minimize" },
            { base: "#FEBC2E", hover: "#DEA123", action: "maximize" },
            { base: "#FF5F57", hover: "#E0443E", action: "close" }
        ]

        delegate: Rectangle {
            width: 12
            height: 12
            radius: 6
            color: area.containsMouse ? modelData.hover : modelData.base
            border.width: 1
            border.color: Qt.rgba(0.0, 0.0, 0.0, 0.09)

            MouseArea {
                id: area
                anchors.fill: parent
                hoverEnabled: true
                onClicked: {
                    if (!root.target) {
                        return
                    }
                    if (modelData.action === "close") {
                        root.target.close()
                    } else if (modelData.action === "minimize") {
                        root.target.showMinimized()
                    } else if (root.target.visibility === Window.Maximized) {
                        root.target.showNormal()
                    } else {
                        root.target.showMaximized()
                    }
                }
            }
        }
    }
}
