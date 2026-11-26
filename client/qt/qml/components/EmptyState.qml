import QtQuick
import Pulse.Client

/// Placeholder shown when a list has no rows. Distinguishing "empty" from
/// "still loading" is the caller's job; this only renders the empty case.
///
/// It extends SelectableText (a read-only TextEdit), so callers set the
/// inherited `text` property directly. It must NOT declare its own `text`,
/// which would shadow the base member.
SelectableText {
    anchors.centerIn: parent
    color: Style.secondaryText
    font.pixelSize: Style.fontSize
    text: qsTr("No data")
}
