import QtQuick

/// Edge and corner drag zones for a frameless window.
///
/// Without native decorations the compositor cannot resize the window from a
/// border, so each visible edge forwards a press to startSystemResize(). The
/// zones are thin (4 px edges, 12 px corners) and sit above the page content.
Item {
    id: root

    /// The ApplicationWindow to resize.
    property var target

    // Left and right edges, insets keep the corners for the diagonal zones.
    MouseArea {
        width: 4
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.topMargin: 12
        anchors.bottomMargin: 12
        cursorShape: Qt.SizeHorCursor
        onPressed: if (root.target) root.target.startSystemResize(Qt.LeftEdge)
    }

    MouseArea {
        width: 4
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.topMargin: 12
        anchors.bottomMargin: 12
        cursorShape: Qt.SizeHorCursor
        onPressed: if (root.target) root.target.startSystemResize(Qt.RightEdge)
    }

    MouseArea {
        height: 4
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        cursorShape: Qt.SizeVerCursor
        onPressed: if (root.target) root.target.startSystemResize(Qt.TopEdge)
    }

    MouseArea {
        height: 4
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        cursorShape: Qt.SizeVerCursor
        onPressed: if (root.target) root.target.startSystemResize(Qt.BottomEdge)
    }

    // Corners.
    MouseArea {
        width: 12; height: 12
        anchors.left: parent.left
        anchors.top: parent.top
        cursorShape: Qt.SizeFDiagCursor
        onPressed: if (root.target) root.target.startSystemResize(Qt.LeftEdge | Qt.TopEdge)
    }
    MouseArea {
        width: 12; height: 12
        anchors.right: parent.right
        anchors.top: parent.top
        cursorShape: Qt.SizeBDiagCursor
        onPressed: if (root.target) root.target.startSystemResize(Qt.RightEdge | Qt.TopEdge)
    }
    MouseArea {
        width: 12; height: 12
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        cursorShape: Qt.SizeBDiagCursor
        onPressed: if (root.target) root.target.startSystemResize(Qt.LeftEdge | Qt.BottomEdge)
    }
    MouseArea {
        width: 12; height: 12
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        cursorShape: Qt.SizeFDiagCursor
        onPressed: if (root.target) root.target.startSystemResize(Qt.RightEdge | Qt.BottomEdge)
    }
}
