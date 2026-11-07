import QtQuick
import Pulse.Client

/// Placeholder shown when a list has no rows. Distinguishing "empty" from
/// "still loading" is the caller's job; this only renders the empty case.
Text {
    property string text: qsTr("No data")

    anchors.centerIn: parent
    color: Style.muted
    font.pixelSize: Style.fontSize
}
