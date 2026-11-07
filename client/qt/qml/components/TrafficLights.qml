import QtQuick
import Pulse.Client

/// The three macOS window controls.
///
/// The window is frameless (see Main.qml), so these carry the real behaviour:
/// close, minimize, and maximize/restore. Colours are the macOS traffic-light
/// values; the subtle border keeps the yellow button visible on a light title
/// bar.
Row {
    id: root

    /// The ApplicationWindow these buttons control.
    property var target

    spacing: 8

    Repeater {
        model: [
            { base: "#FF5F57", hover: "#E0443E", action: "close" },
            { base: "#FEBC2E", hover: "#DEA123", action: "minimize" },
            { base: "#28C840", hover: "#1DAD35", action: "maximize" }
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
